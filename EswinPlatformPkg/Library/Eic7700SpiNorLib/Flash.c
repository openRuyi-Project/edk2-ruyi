/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Guid/FdtHob.h>
#include <IndustryStandard/SpiNorFlashJedecSfdp.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/Eic7700SpiNorLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

// EIC7700 Boot SPI has a CPU-addressable 256-byte transfer window.
// This is separate from the SoC's DesignWare general-purpose SPI ports.
#define BOOT_SPI_BASE       0x51800000U
#define BOOT_SPI_DATA       0x5c000000U
#define SYSCRG_BASE         0x51828000U
#define SPI_CLOCK           0x104
#define SPI_CONFIG_CLOCK    0x108
#define AON_BUS_CLOCK       0x1e4
#define AON_BUS_ENABLE      BIT29
#define SPI_RESET           0x490
#define CLOCK_ENABLE        BIT31
#define SPI_CLOCK_XTAL       BIT0
#define SPI_RESETS_RELEASED  (BIT0 | BIT1)

#define STATUS_WRITE_TIME   0x00
#define BUS_MODE            0x04
#define FAST_READ           0x10
#define TRANSFER_BYTES      0x14
#define COMMAND             0x18
#define INTERRUPT_CONTROL   0x1c
#define DMA_THRESHOLD       0x20
#define FLASH_ADDRESS       0x24
#define PAGE_PROGRAM_TIME   0x28
#define SECTOR_ERASE_TIME   0x2c
#define BLOCK_ERASE_TIME    0x34
#define COMMAND_VALID       BIT0
#define COMMAND_KIND_SHIFT  1
#define COMMAND_OPCODE_SHIFT 6
#define INTERRUPT_DONE      BIT0
#define INTERRUPT_CLEAR     BIT1
#define INTERRUPT_ENABLE    BIT2
#define PAGE_BYTES          SPI_FLASH_PAGE_SIZE
#define TIMEOUT_US          3000000U

typedef enum {
  Program = 0,
  ReadStatus = 2,
  EraseSector = 3,
  EraseBlock = 5,
  ReadData = 13,
  ReadJedecId = 15
} COMMAND_KIND;

STATIC UINTN   mSpi = BOOT_SPI_BASE, mData = BOOT_SPI_DATA, mClock = SYSCRG_BASE;
STATIC UINT32  mSize;
STATIC BOOLEAN mReady, mFaulted, mBusy, mAtRuntime;
STATIC EFI_EVENT mExitEvent;
STATIC CONST UINT32 mRegisters[] = {
  STATUS_WRITE_TIME, BUS_MODE, FAST_READ, TRANSFER_BYTES, DMA_THRESHOLD,
  FLASH_ADDRESS, PAGE_PROGRAM_TIME, SECTOR_ERASE_TIME, BLOCK_ERASE_TIME
};

typedef struct {
  UINT32  Clock;
  UINT32  ConfigClock;
  UINT32  BusClock;
  UINT32  Interrupt;
  UINT32  Registers[ARRAY_SIZE (mRegisters)];
  EFI_TPL Tpl;
  BOOLEAN BootServices;
  BOOLEAN Saved;
} SPI_ACCESS;

STATIC EFI_STATUS
WaitCommand (VOID)
{
  UINT64 Start = GetPerformanceCounter ();

  while (MmioRead32 (mSpi + COMMAND) & COMMAND_VALID) {
    if (GetTimeInNanoSecond (GetPerformanceCounter () - Start) >= TIMEOUT_US * 1000ULL) {
      mFaulted = TRUE;
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
WaitCompletion (VOID)
{
  UINT64 Start = GetPerformanceCounter ();

  while (!(MmioRead32 (mSpi + INTERRUPT_CONTROL) & INTERRUPT_DONE)) {
    if (GetTimeInNanoSecond (GetPerformanceCounter () - Start) >= TIMEOUT_US * 1000ULL) {
      mFaulted = TRUE;
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Execute (UINT8 Opcode, COMMAND_KIND Kind, UINT32 Offset,
         UINTN Size, CONST UINT8 *Output, UINT8 *Input)
{
  EFI_STATUS Status;
  UINTN Index, Bytes;
  UINT32 Word;

  Status = WaitCommand ();
  if (EFI_ERROR (Status)) {
    return Status;
  }
  MmioWrite32 (mSpi + INTERRUPT_CONTROL, INTERRUPT_CLEAR | INTERRUPT_ENABLE);
  MmioWrite32 (mSpi + FLASH_ADDRESS, Offset);
  MmioWrite32 (mSpi + TRANSFER_BYTES, (UINT32)Size);
  if (Output != NULL) {
    for (Index = 0; Index < Size; Index += sizeof (Word)) {
      Word = MAX_UINT32;
      Bytes = MIN (Size - Index, sizeof (Word));
      CopyMem (&Word, Output + Index, Bytes);
      MmioWrite32 (mData + Index, Word);
    }
  }
  MemoryFence ();
  // The move-mode bit stays clear. No system DMA channel is used.
  MmioWrite32 (mSpi + COMMAND, ((UINT32)Opcode << COMMAND_OPCODE_SHIFT) |
               ((UINT32)Kind << COMMAND_KIND_SHIFT) | COMMAND_VALID);
  Status = WaitCommand ();
  if (!EFI_ERROR (Status)) { Status = WaitCompletion (); }
  if (!EFI_ERROR (Status) && Input != NULL) {
    for (Index = 0; Index < Size; Index += sizeof (Word)) {
      Word = MmioRead32 (mData + Index);
      CopyMem (Input + Index, &Word, MIN (Size - Index, sizeof (Word)));
    }
  }
  return Status;
}

STATIC EFI_STATUS
WaitFlash (VOID)
{
  UINT64 Start = GetPerformanceCounter ();
  EFI_STATUS Status;
  UINT8 Value;

  do {
    Status = Execute (SPI_FLASH_RDSR, ReadStatus, 0, 1, NULL, &Value);
    if (EFI_ERROR (Status) || !(Value & BIT0)) {
      return Status;
    }
    MicroSecondDelay (10);
  } while (GetTimeInNanoSecond (GetPerformanceCounter () - Start) < TIMEOUT_US * 1000ULL);
  mFaulted = TRUE;
  return EFI_TIMEOUT;
}

STATIC VOID
EndAccess (SPI_ACCESS *Access)
{
  UINTN Index;

  // An incomplete command must retain its clocks and register state.
  if (!mFaulted && !(MmioRead32 (mSpi + COMMAND) & COMMAND_VALID)) {
    if (Access->Saved) {
      for (Index = 0; Index < ARRAY_SIZE (mRegisters); Index++) {
        MmioWrite32 (mSpi + mRegisters[Index], Access->Registers[Index]);
      }
      MmioWrite32 (mSpi + INTERRUPT_CONTROL, Access->Interrupt | INTERRUPT_CLEAR);
    }
    MmioWrite32 (mClock + SPI_CLOCK, Access->Clock);
    MmioWrite32 (mClock + SPI_CONFIG_CLOCK, Access->ConfigClock);
    MmioAndThenOr32 (mClock + AON_BUS_CLOCK, ~((UINT32)AON_BUS_ENABLE),
                     Access->BusClock & AON_BUS_ENABLE);
  }
  mBusy = FALSE;
  if (Access->BootServices) {
    gBS->RestoreTPL (Access->Tpl);
  }
}

STATIC EFI_STATUS
BeginAccess (SPI_ACCESS *Access)
{
  EFI_STATUS Status;
  UINTN Index;

  if (!mReady || mFaulted) {
    return EFI_NOT_READY;
  }
  ZeroMem (Access, sizeof (*Access));
  Access->BootServices = !mAtRuntime;
  if (Access->BootServices) {
    Access->Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  }
  if (mBusy) {
    if (Access->BootServices) { gBS->RestoreTPL (Access->Tpl); }
    return EFI_ALREADY_STARTED;
  }
  mBusy = TRUE;
  Access->Clock = MmioRead32 (mClock + SPI_CLOCK);
  Access->ConfigClock = MmioRead32 (mClock + SPI_CONFIG_CLOCK);
  Access->BusClock = MmioRead32 (mClock + AON_BUS_CLOCK);
  // The PIO data window requires the AON AXI fabric clock. This gate is
  // separate from the DMA engine and SPI interface/configuration clocks.
  MmioOr32 (mClock + AON_BUS_CLOCK, AON_BUS_ENABLE);
  MmioWrite32 (mClock + SPI_CONFIG_CLOCK, Access->ConfigClock | CLOCK_ENABLE);
  MmioWrite32 (mClock + SPI_CLOCK, Access->Clock | CLOCK_ENABLE);
  if ((MmioRead32 (mClock + SPI_RESET) & SPI_RESETS_RELEASED) != SPI_RESETS_RELEASED) {
    MmioWrite32 (mClock + SPI_CLOCK, Access->Clock);
    MmioWrite32 (mClock + SPI_CONFIG_CLOCK, Access->ConfigClock);
    MmioAndThenOr32 (mClock + AON_BUS_CLOCK, ~((UINT32)AON_BUS_ENABLE),
                     Access->BusClock & AON_BUS_ENABLE);
    mBusy = FALSE;
    if (Access->BootServices) { gBS->RestoreTPL (Access->Tpl); }
    return EFI_NOT_READY;
  }
  Status = WaitCommand ();
  if (EFI_ERROR (Status)) {
    EndAccess (Access);
    return Status;
  }
  for (Index = 0; Index < ARRAY_SIZE (mRegisters); Index++) {
    Access->Registers[Index] = MmioRead32 (mSpi + mRegisters[Index]);
  }
  Access->Interrupt = MmioRead32 (mSpi + INTERRUPT_CONTROL) & INTERRUPT_ENABLE;
  Access->Saved = TRUE;
  MmioWrite32 (mClock + SPI_CLOCK, Access->Clock | CLOCK_ENABLE | SPI_CLOCK_XTAL);
  MmioWrite32 (mSpi + BUS_MODE, 0);
  MmioWrite32 (mSpi + FAST_READ, 0);
  MmioWrite32 (mSpi + DMA_THRESHOLD, 0);
  // Complete controller framing promptly, then poll the flash WIP bit.
  // Fixed worst-case delays penalize every page and every erase operation.
  MmioWrite32 (mSpi + PAGE_PROGRAM_TIME, 1);
  MmioWrite32 (mSpi + SECTOR_ERASE_TIME, 1);
  MmioWrite32 (mSpi + BLOCK_ERASE_TIME, 1);
  Status = WaitFlash ();
  if (EFI_ERROR (Status)) {
    EndAccess (Access);
  }
  return Status;
}

STATIC EFI_STATUS
BlockLock (UINT32 Offset, BOOLEAN Locked)
{
  EFI_STATUS Status;
  UINT8 Value;

  // Lock bits latch on CS rising. The block framing supplies WREN and the
  // address, but these commands must not wait for a 64 KiB array erase.
  Status = Execute (Locked ? 0x36 : 0x39, EraseBlock, Offset, 0, NULL, NULL);
  if (!EFI_ERROR (Status)) { Status = WaitFlash (); }
  if (!EFI_ERROR (Status)) { Status = Execute (0x3d, ReadData, Offset, 1, NULL, &Value); }
  if (!EFI_ERROR (Status) && !!(Value & BIT0) != Locked) { Status = EFI_WRITE_PROTECTED; }
  return Status;
}

STATIC EFI_STATUS
Unlock (UINT32 Offset, BOOLEAN *Relock)
{
  EFI_STATUS Status;
  UINT8 Sr1, Sr2, Sr3, Value;

  *Relock = FALSE;
  Status = Execute (0x15, ReadStatus, 0, 1, NULL, &Sr3);
  if (EFI_ERROR (Status)) { return Status; }
  if (Sr3 & BIT2) {
    Status = Execute (0x3d, ReadData, Offset, 1, NULL, &Value);
    if (EFI_ERROR (Status)) { return Status; }
    *Relock = !!(Value & BIT0);
    return *Relock ? BlockLock (Offset, FALSE) : EFI_SUCCESS;
  }
  Status = Execute (SPI_FLASH_RDSR, ReadStatus, 0, 1, NULL, &Sr1);
  if (!EFI_ERROR (Status)) { Status = Execute (0x35, ReadStatus, 0, 1, NULL, &Sr2); }
  if (EFI_ERROR (Status)) { return Status; }
  // Do not modify the caller's nonvolatile protection or quad-enable bits.
  return ((Sr1 & (BIT2 | BIT3 | BIT4)) || (Sr2 & BIT6)) ? EFI_WRITE_PROTECTED : EFI_SUCCESS;
}

UINT32
Eic7700SpiNorSize (VOID)
{
  return mReady ? mSize : 0;
}

EFI_STATUS
Eic7700SpiNorRead (UINT32 Offset, UINTN Size, VOID *Buffer)
{
  SPI_ACCESS Access;
  EFI_STATUS Status;
  UINT8 *Data = Buffer;
  UINTN Chunk;

  if (!Buffer || !Size || (Offset >= mSize) || (Size > mSize - Offset)) {
    return EFI_INVALID_PARAMETER;
  }
  Status = BeginAccess (&Access);
  if (EFI_ERROR (Status)) { return Status; }
  while (Size != 0) {
    Chunk = MIN (Size, PAGE_BYTES);
    Status = Execute (SPI_FLASH_READ, ReadData, Offset, Chunk, NULL, Data);
    if (EFI_ERROR (Status)) { break; }
    Data += Chunk;
    Offset += Chunk;
    Size -= Chunk;
  }
  EndAccess (&Access);
  return Status;
}

EFI_STATUS
Eic7700SpiNorWrite (UINT32 Offset, UINTN Size, CONST VOID *Buffer)
{
  SPI_ACCESS Access;
  EFI_STATUS Status, LockStatus;
  BOOLEAN Relock;
  UINT8 Verify[PAGE_BYTES];

  if (!Buffer || !Size || (Offset >= mSize) || (Size > mSize - Offset) ||
      (Size > PAGE_BYTES - Offset % PAGE_BYTES))
  {
    return EFI_INVALID_PARAMETER;
  }
  Status = BeginAccess (&Access);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Unlock (Offset, &Relock);
  if (!EFI_ERROR (Status)) { Status = Execute (SPI_FLASH_PP, Program, Offset, Size, Buffer, NULL); }
  if (!EFI_ERROR (Status)) { Status = WaitFlash (); }
  if (!EFI_ERROR (Status)) { Status = Execute (SPI_FLASH_READ, ReadData, Offset, Size, NULL, Verify); }
  if (!EFI_ERROR (Status) && CompareMem (Verify, Buffer, Size)) { Status = EFI_DEVICE_ERROR; }
  if (Relock && !mFaulted) {
    LockStatus = BlockLock (Offset, TRUE);
    if (EFI_ERROR (LockStatus)) { Status = LockStatus; }
  }
  EndAccess (&Access);
  return Status;
}

EFI_STATUS
Eic7700SpiNorErase (UINT32 Offset, UINTN Size)
{
  SPI_ACCESS Access;
  EFI_STATUS Status, LockStatus;
  BOOLEAN Relock;
  UINT8 Verify[PAGE_BYTES];
  UINTN Index, Byte;

  if (((Size != SIZE_4KB) && (Size != SIZE_64KB)) || (Offset % Size) ||
      (Offset >= mSize) || (Size > mSize - Offset))
  {
    return EFI_INVALID_PARAMETER;
  }
  // The first and last 64 KiB use sixteen independent 4 KiB lock bits.
  // Erase those blocks sector by sector to preserve each original lock.
  if (Size == SIZE_64KB && (Offset == 0 || Offset == mSize - SIZE_64KB)) {
    for (Index = 0; Index < Size; Index += SIZE_4KB) {
      Status = Eic7700SpiNorErase (Offset + Index, SIZE_4KB);
      if (EFI_ERROR (Status)) { return Status; }
    }
    return EFI_SUCCESS;
  }
  Status = BeginAccess (&Access);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Unlock (Offset, &Relock);
  if (!EFI_ERROR (Status)) {
    Status = Execute (Size == SIZE_4KB ? SPI_FLASH_SE : SPI_FLASH_BE,
                      Size == SIZE_4KB ? EraseSector : EraseBlock, Offset, 0, NULL, NULL);
  }
  if (!EFI_ERROR (Status)) { Status = WaitFlash (); }
  for (Index = 0; !EFI_ERROR (Status) && Index < Size; Index += PAGE_BYTES) {
    Status = Execute (SPI_FLASH_READ, ReadData, Offset + Index, sizeof (Verify), NULL, Verify);
    if (EFI_ERROR (Status)) { break; }
    for (Byte = 0; Byte < sizeof (Verify); Byte++) {
      if (Verify[Byte] != 0xff) { Status = EFI_DEVICE_ERROR; break; }
    }
  }
  if (Relock && !mFaulted) {
    LockStatus = BlockLock (Offset, TRUE);
    if (EFI_ERROR (LockStatus)) { Status = LockStatus; }
  }
  EndAccess (&Access);
  return Status;
}

STATIC EFI_STATUS
MapPage (UINTN Address, BOOLEAN Runtime)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor;
  EFI_STATUS Status;

  Status = gDS->GetMemorySpaceDescriptor (Address, &Descriptor);
  if (EFI_ERROR (Status)) { return Status; }
  if (Descriptor.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Address, EFI_PAGE_SIZE,
                                 EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
    if (EFI_ERROR (Status)) { return Status; }
  } else if ((Descriptor.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) ||
             Descriptor.BaseAddress > Address || Descriptor.Length < EFI_PAGE_SIZE ||
             Address - Descriptor.BaseAddress > Descriptor.Length - EFI_PAGE_SIZE)
  {
    return EFI_UNSUPPORTED;
  }
  return gDS->SetMemorySpaceAttributes (Address, EFI_PAGE_SIZE, EFI_MEMORY_UC |
           ((Runtime ? EFI_MEMORY_RUNTIME : 0) | (Descriptor.Attributes & EFI_MEMORY_RUNTIME)));
}

STATIC VOID EFIAPI
ExitBootServices (EFI_EVENT Event, VOID *Context)
{
  mAtRuntime = TRUE;
}

EFI_STATUS EFIAPI
Eic7700SpiNorDestructor (VOID)
{
  if ((mExitEvent != NULL) && !mAtRuntime) {
    gBS->CloseEvent (mExitEvent);
    mExitEvent = NULL;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
Eic7700SpiNorInit (BOOLEAN Runtime)
{
  EFI_HOB_GUID_TYPE *Hob;
  CONST VOID *Fdt;
  EFI_STATUS Status;
  SPI_ACCESS Access;
  UINT8 Id[3];

  Hob = GetFirstGuidHob (&gFdtHobGuid);
  if (!Hob || GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (UINT64)) { return EFI_NOT_FOUND; }
  Fdt = (VOID *)(UINTN)ReadUnaligned64 (GET_GUID_HOB_DATA (Hob));
  if (!Fdt || FdtCheckHeader (Fdt)) { return EFI_COMPROMISED_DATA; }
  if (FdtStringListSearch (Fdt, 0, "compatible", "eswin,eic7700") < 0) {
    return EFI_UNSUPPORTED;
  }
  Status = MapPage (mSpi, Runtime);
  if (!EFI_ERROR (Status)) { Status = MapPage (mData, Runtime); }
  if (!EFI_ERROR (Status)) { Status = MapPage (mClock, Runtime); }
  if (EFI_ERROR (Status)) { return Status; }
  mReady = TRUE;
  Status = BeginAccess (&Access);
  if (!EFI_ERROR (Status)) {
    Status = Execute (SPI_FLASH_RDID, ReadJedecId, 0, sizeof (Id), NULL, Id);
    EndAccess (&Access);
  }
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "EIC7700 Boot SPI ID: %02x %02x %02x\n", Id[0], Id[1], Id[2]));
  }
  // Winbond W25Q64JW/W25Q128JW share three-byte addressing and lock commands.
  if (!EFI_ERROR (Status) &&
      ((Id[0] != 0xef) || (Id[1] != 0x60) || ((Id[2] != 0x17) && (Id[2] != 0x18))))
  {
    Status = EFI_UNSUPPORTED;
  }
  if (!EFI_ERROR (Status) && Runtime && (mExitEvent == NULL)) {
    Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ExitBootServices,
                                 NULL, &gEfiEventExitBootServicesGuid, &mExitEvent);
  }
  mReady = !EFI_ERROR (Status);
  if (mReady) { mSize = 1U << Id[2]; }
  return Status;
}

VOID
Eic7700SpiNorVirtualAddressChange (VOID)
{
  if (EFI_ERROR (gRT->ConvertPointer (0, (VOID **)&mSpi)) ||
      EFI_ERROR (gRT->ConvertPointer (0, (VOID **)&mData)) ||
      EFI_ERROR (gRT->ConvertPointer (0, (VOID **)&mClock)))
  {
    mReady = FALSE;
  }
}
