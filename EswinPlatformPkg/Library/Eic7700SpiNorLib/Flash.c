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

STATIC EIC7700_SPI_NOR mLocal = {
  .Spi = BOOT_SPI_BASE, .Data = BOOT_SPI_DATA, .Clock = SYSCRG_BASE
};
STATIC BOOLEAN mAtRuntime;
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

// Forward die 1 transactions to the selected hart.
STATIC UINT32
ReadReg (EIC7700_SPI_NOR *Nor, UINTN Address)
{
  UINT32 Value = 0;

  if (Nor->Transfer == NULL) {
    return MmioRead32 (Address);
  }
  if (!EFI_ERROR (Nor->IoStatus)) {
    Nor->IoStatus = Nor->Transfer (Nor->Context, FALSE, Address, &Value, 1);
  }
  return Value;
}

STATIC VOID
WriteReg (EIC7700_SPI_NOR *Nor, UINTN Address, UINT32 Value)
{
  if (Nor->Transfer == NULL) {
    MmioWrite32 (Address, Value);
  } else if (!EFI_ERROR (Nor->IoStatus)) {
    Nor->IoStatus = Nor->Transfer (Nor->Context, TRUE, Address, &Value, 1);
  }
}

STATIC EFI_STATUS
WaitCommand (EIC7700_SPI_NOR *Nor)
{
  UINT64 Start = GetPerformanceCounter ();

  while (ReadReg (Nor, Nor->Spi + COMMAND) & COMMAND_VALID) {
    if (GetTimeInNanoSecond (GetPerformanceCounter () - Start) >= TIMEOUT_US * 1000ULL) {
      Nor->Faulted = TRUE;
      return EFI_TIMEOUT;
    }
    if (EFI_ERROR (Nor->IoStatus)) { return Nor->IoStatus; }
    MicroSecondDelay (1);
  }
  return Nor->IoStatus;
}

STATIC EFI_STATUS
WaitCompletion (EIC7700_SPI_NOR *Nor)
{
  UINT64 Start = GetPerformanceCounter ();

  while (!(ReadReg (Nor, Nor->Spi + INTERRUPT_CONTROL) & INTERRUPT_DONE)) {
    if (GetTimeInNanoSecond (GetPerformanceCounter () - Start) >= TIMEOUT_US * 1000ULL) {
      Nor->Faulted = TRUE;
      return EFI_TIMEOUT;
    }
    if (EFI_ERROR (Nor->IoStatus)) { return Nor->IoStatus; }
    MicroSecondDelay (1);
  }
  return Nor->IoStatus;
}

STATIC VOID
TransferWindow (EIC7700_SPI_NOR *Nor, UINTN Size, CONST UINT8 *Output, UINT8 *Input)
{
  UINT32 Words[PAGE_BYTES / sizeof (UINT32)];
  UINTN Count = ALIGN_VALUE (Size, sizeof (UINT32)) / sizeof (UINT32);

  if (Output != NULL) {
    Words[Count - 1] = MAX_UINT32;
    CopyMem (Words, Output, Size);
  }
  if (Nor->Transfer != NULL) {
    if (!EFI_ERROR (Nor->IoStatus)) {
      Nor->IoStatus = Nor->Transfer (Nor->Context, Output != NULL, Nor->Data, Words, Count);
    }
  } else {
    for (UINTN Index = 0; Index < Count; ++Index) {
      if (Output != NULL) {
        MmioWrite32 (Nor->Data + Index * sizeof (UINT32), Words[Index]);
      } else {
        Words[Index] = MmioRead32 (Nor->Data + Index * sizeof (UINT32));
      }
    }
  }
  if ((Input != NULL) && !EFI_ERROR (Nor->IoStatus)) {
    CopyMem (Input, Words, Size);
  }
}

STATIC EFI_STATUS
Execute (EIC7700_SPI_NOR *Nor, UINT8 Opcode, COMMAND_KIND Kind, UINT32 Offset,
         UINTN Size, CONST UINT8 *Output, UINT8 *Input)
{
  EFI_STATUS Status;
  if (Size > PAGE_BYTES || ((Output != NULL || Input != NULL) && Size == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = WaitCommand (Nor);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  WriteReg (Nor, Nor->Spi + INTERRUPT_CONTROL, INTERRUPT_CLEAR | INTERRUPT_ENABLE);
  WriteReg (Nor, Nor->Spi + FLASH_ADDRESS, Offset);
  WriteReg (Nor, Nor->Spi + TRANSFER_BYTES, (UINT32)Size);
  if (Output != NULL) {
    TransferWindow (Nor, Size, Output, NULL);
  }
  MemoryFence ();
  // The move-mode bit stays clear. No system DMA channel is used.
  WriteReg (Nor, Nor->Spi + COMMAND, ((UINT32)Opcode << COMMAND_OPCODE_SHIFT) |
               ((UINT32)Kind << COMMAND_KIND_SHIFT) | COMMAND_VALID);
  Status = WaitCommand (Nor);
  if (!EFI_ERROR (Status)) { Status = WaitCompletion (Nor); }
  if (!EFI_ERROR (Status) && Input != NULL) {
    TransferWindow (Nor, Size, NULL, Input);
  }
  return EFI_ERROR (Nor->IoStatus) ? Nor->IoStatus : Status;
}

STATIC EFI_STATUS
WaitFlash (EIC7700_SPI_NOR *Nor)
{
  UINT64 Start = GetPerformanceCounter ();
  EFI_STATUS Status;
  UINT8 Value;

  do {
    Status = Execute (Nor, SPI_FLASH_RDSR, ReadStatus, 0, 1, NULL, &Value);
    if (EFI_ERROR (Status) || !(Value & BIT0)) {
      return Status;
    }
    MicroSecondDelay (10);
  } while (GetTimeInNanoSecond (GetPerformanceCounter () - Start) < TIMEOUT_US * 1000ULL);
  Nor->Faulted = TRUE;
  return EFI_TIMEOUT;
}

STATIC VOID
EndAccess (EIC7700_SPI_NOR *Nor, SPI_ACCESS *Access)
{
  UINTN Index;

  // An incomplete command must retain its clocks and register state.
  if (!Nor->Faulted && !(ReadReg (Nor, Nor->Spi + COMMAND) & COMMAND_VALID)) {
    if (Access->Saved) {
      for (Index = 0; Index < ARRAY_SIZE (mRegisters); Index++) {
        WriteReg (Nor, Nor->Spi + mRegisters[Index], Access->Registers[Index]);
      }
      WriteReg (Nor, Nor->Spi + INTERRUPT_CONTROL, Access->Interrupt | INTERRUPT_CLEAR);
    }
    WriteReg (Nor, Nor->Clock + SPI_CLOCK, Access->Clock);
    WriteReg (Nor, Nor->Clock + SPI_CONFIG_CLOCK, Access->ConfigClock);
    WriteReg (Nor, Nor->Clock + AON_BUS_CLOCK,
              (ReadReg (Nor, Nor->Clock + AON_BUS_CLOCK) & ~(UINT32)AON_BUS_ENABLE) |
              (Access->BusClock & AON_BUS_ENABLE));
  }
  Nor->Busy = FALSE;
  if (Access->BootServices) {
    gBS->RestoreTPL (Access->Tpl);
  }
}

STATIC EFI_STATUS
BeginAccess (EIC7700_SPI_NOR *Nor, SPI_ACCESS *Access)
{
  EFI_STATUS Status;
  UINTN Index;

  if (!Nor->Ready || Nor->Faulted) {
    return EFI_NOT_READY;
  }
  ZeroMem (Access, sizeof (*Access));
  Access->BootServices = !mAtRuntime && (Nor == &mLocal);
  if (Access->BootServices) {
    Access->Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  }
  if (Nor->Busy) {
    if (Access->BootServices) { gBS->RestoreTPL (Access->Tpl); }
    return EFI_ALREADY_STARTED;
  }
  Nor->Busy = TRUE;
  Access->Clock = ReadReg (Nor, Nor->Clock + SPI_CLOCK);
  Access->ConfigClock = ReadReg (Nor, Nor->Clock + SPI_CONFIG_CLOCK);
  Access->BusClock = ReadReg (Nor, Nor->Clock + AON_BUS_CLOCK);
  // The PIO data window requires the AON AXI fabric clock. This gate is
  // separate from the DMA engine and SPI interface/configuration clocks.
  WriteReg (Nor, Nor->Clock + AON_BUS_CLOCK, ReadReg (Nor, Nor->Clock + AON_BUS_CLOCK) | AON_BUS_ENABLE);
  WriteReg (Nor, Nor->Clock + SPI_CONFIG_CLOCK, Access->ConfigClock | CLOCK_ENABLE);
  WriteReg (Nor, Nor->Clock + SPI_CLOCK, Access->Clock | CLOCK_ENABLE);
  if ((ReadReg (Nor, Nor->Clock + SPI_RESET) & SPI_RESETS_RELEASED) != SPI_RESETS_RELEASED) {
    WriteReg (Nor, Nor->Clock + SPI_CLOCK, Access->Clock);
    WriteReg (Nor, Nor->Clock + SPI_CONFIG_CLOCK, Access->ConfigClock);
    WriteReg (Nor, Nor->Clock + AON_BUS_CLOCK,
              (ReadReg (Nor, Nor->Clock + AON_BUS_CLOCK) & ~(UINT32)AON_BUS_ENABLE) |
              (Access->BusClock & AON_BUS_ENABLE));
    Nor->Busy = FALSE;
    if (Access->BootServices) { gBS->RestoreTPL (Access->Tpl); }
    return EFI_ERROR (Nor->IoStatus) ? Nor->IoStatus : EFI_NOT_READY;
  }
  Status = WaitCommand (Nor);
  if (EFI_ERROR (Status)) {
    EndAccess (Nor, Access);
    return Status;
  }
  for (Index = 0; Index < ARRAY_SIZE (mRegisters); Index++) {
    Access->Registers[Index] = ReadReg (Nor, Nor->Spi + mRegisters[Index]);
  }
  Access->Interrupt = ReadReg (Nor, Nor->Spi + INTERRUPT_CONTROL) & INTERRUPT_ENABLE;
  Access->Saved = TRUE;
  WriteReg (Nor, Nor->Clock + SPI_CLOCK, Access->Clock | CLOCK_ENABLE | SPI_CLOCK_XTAL);
  WriteReg (Nor, Nor->Spi + BUS_MODE, 0);
  WriteReg (Nor, Nor->Spi + FAST_READ, 0);
  WriteReg (Nor, Nor->Spi + DMA_THRESHOLD, 0);
  // Complete controller framing promptly, then poll the flash WIP bit.
  // Fixed worst-case delays penalize every page and every erase operation.
  WriteReg (Nor, Nor->Spi + PAGE_PROGRAM_TIME, 1);
  WriteReg (Nor, Nor->Spi + SECTOR_ERASE_TIME, 1);
  WriteReg (Nor, Nor->Spi + BLOCK_ERASE_TIME, 1);
  Status = WaitFlash (Nor);
  if (EFI_ERROR (Status)) {
    EndAccess (Nor, Access);
  }
  return Status;
}

STATIC EFI_STATUS
BlockLock (EIC7700_SPI_NOR *Nor, UINT32 Offset, BOOLEAN Locked)
{
  EFI_STATUS Status;
  UINT8 Value;

  // Lock bits latch on CS rising. The block framing supplies WREN and the
  // address, but these commands must not wait for a 64 KiB array erase.
  Status = Execute (Nor, Locked ? 0x36 : 0x39, EraseBlock, Offset, 0, NULL, NULL);
  if (!EFI_ERROR (Status)) { Status = WaitFlash (Nor); }
  if (!EFI_ERROR (Status)) { Status = Execute (Nor, 0x3d, ReadData, Offset, 1, NULL, &Value); }
  if (!EFI_ERROR (Status) && !!(Value & BIT0) != Locked) { Status = EFI_WRITE_PROTECTED; }
  return Status;
}

STATIC EFI_STATUS
Unlock (EIC7700_SPI_NOR *Nor, UINT32 Offset, BOOLEAN *Relock)
{
  EFI_STATUS Status;
  UINT8 Sr1, Sr2, Sr3, Value;

  *Relock = FALSE;
  // GD25LQ128 uses SR1/SR2 block protection, not Winbond individual locks.
  if (!Nor->Gd25Lq128) {
    Status = Execute (Nor, 0x15, ReadStatus, 0, 1, NULL, &Sr3);
    if (EFI_ERROR (Status)) { return Status; }
    if (Sr3 & BIT2) {
      Status = Execute (Nor, 0x3d, ReadData, Offset, 1, NULL, &Value);
      if (EFI_ERROR (Status)) { return Status; }
      *Relock = !!(Value & BIT0);
      return *Relock ? BlockLock (Nor, Offset, FALSE) : EFI_SUCCESS;
    }
  }
  Status = Execute (Nor, SPI_FLASH_RDSR, ReadStatus, 0, 1, NULL, &Sr1);
  if (!EFI_ERROR (Status)) { Status = Execute (Nor, 0x35, ReadStatus, 0, 1, NULL, &Sr2); }
  if (EFI_ERROR (Status)) { return Status; }
  // Do not modify the caller's nonvolatile protection or quad-enable bits.
  return ((Sr1 & (BIT2 | BIT3 | BIT4)) || (Sr2 & BIT6)) ? EFI_WRITE_PROTECTED : EFI_SUCCESS;
}

EFI_STATUS
Eic7700SpiNorReadDevice (EIC7700_SPI_NOR *Nor, UINT32 Offset, UINTN Size, VOID *Buffer)
{
  SPI_ACCESS Access;
  EFI_STATUS Status;
  UINT8 *Data = Buffer;
  UINTN Chunk;

  if (!Buffer || !Size || (Offset >= Nor->Size) || (Size > Nor->Size - Offset)) {
    return EFI_INVALID_PARAMETER;
  }
  Status = BeginAccess (Nor, &Access);
  if (EFI_ERROR (Status)) { return Status; }
  while (Size != 0) {
    Chunk = MIN (Size, PAGE_BYTES);
    Status = Execute (Nor, SPI_FLASH_READ, ReadData, Offset, Chunk, NULL, Data);
    if (EFI_ERROR (Status)) { break; }
    Data += Chunk;
    Offset += Chunk;
    Size -= Chunk;
  }
  EndAccess (Nor, &Access);
  return EFI_ERROR (Nor->IoStatus) ? Nor->IoStatus : Status;
}

EFI_STATUS
Eic7700SpiNorWriteDevice (EIC7700_SPI_NOR *Nor, UINT32 Offset, UINTN Size, CONST VOID *Buffer)
{
  SPI_ACCESS Access;
  EFI_STATUS Status, LockStatus;
  BOOLEAN Relock;
  UINT8 Verify[PAGE_BYTES];

  if (!Buffer || !Size || (Offset >= Nor->Size) || (Size > Nor->Size - Offset) ||
      (Size > PAGE_BYTES - Offset % PAGE_BYTES))
  {
    return EFI_INVALID_PARAMETER;
  }
  Status = BeginAccess (Nor, &Access);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Unlock (Nor, Offset, &Relock);
  if (!EFI_ERROR (Status)) { Status = Execute (Nor, SPI_FLASH_PP, Program, Offset, Size, Buffer, NULL); }
  if (!EFI_ERROR (Status)) { Status = WaitFlash (Nor); }
  if (!EFI_ERROR (Status)) { Status = Execute (Nor, SPI_FLASH_READ, ReadData, Offset, Size, NULL, Verify); }
  if (!EFI_ERROR (Status) && CompareMem (Verify, Buffer, Size)) { Status = EFI_DEVICE_ERROR; }
  if (Relock && !Nor->Faulted) {
    LockStatus = BlockLock (Nor, Offset, TRUE);
    if (EFI_ERROR (LockStatus)) { Status = LockStatus; }
  }
  EndAccess (Nor, &Access);
  return EFI_ERROR (Nor->IoStatus) ? Nor->IoStatus : Status;
}

EFI_STATUS
Eic7700SpiNorEraseDevice (EIC7700_SPI_NOR *Nor, UINT32 Offset, UINTN Size)
{
  SPI_ACCESS Access;
  EFI_STATUS Status, LockStatus;
  BOOLEAN Relock;
  UINT8 Verify[PAGE_BYTES];
  UINTN Index, Byte;

  if (((Size != SIZE_4KB) && (Size != SIZE_64KB)) || (Offset % Size) ||
      (Offset >= Nor->Size) || (Size > Nor->Size - Offset))
  {
    return EFI_INVALID_PARAMETER;
  }
  // The first and last 64 KiB use sixteen independent 4 KiB lock bits.
  // Erase those blocks sector by sector to preserve each original lock.
  if (Size == SIZE_64KB && (Offset == 0 || Offset == Nor->Size - SIZE_64KB)) {
    for (Index = 0; Index < Size; Index += SIZE_4KB) {
      Status = Eic7700SpiNorEraseDevice (Nor, Offset + Index, SIZE_4KB);
      if (EFI_ERROR (Status)) { return Status; }
    }
    return EFI_SUCCESS;
  }
  Status = BeginAccess (Nor, &Access);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Unlock (Nor, Offset, &Relock);
  if (!EFI_ERROR (Status)) {
    Status = Execute (Nor, Size == SIZE_4KB ? SPI_FLASH_SE : SPI_FLASH_BE,
                      Size == SIZE_4KB ? EraseSector : EraseBlock, Offset, 0, NULL, NULL);
  }
  if (!EFI_ERROR (Status)) { Status = WaitFlash (Nor); }
  for (Index = 0; !EFI_ERROR (Status) && Index < Size; Index += PAGE_BYTES) {
    Status = Execute (Nor, SPI_FLASH_READ, ReadData, Offset + Index, sizeof (Verify), NULL, Verify);
    if (EFI_ERROR (Status)) { break; }
    for (Byte = 0; Byte < sizeof (Verify); Byte++) {
      if (Verify[Byte] != 0xff) { Status = EFI_DEVICE_ERROR; break; }
    }
  }
  if (Relock && !Nor->Faulted) {
    LockStatus = BlockLock (Nor, Offset, TRUE);
    if (EFI_ERROR (LockStatus)) { Status = LockStatus; }
  }
  EndAccess (Nor, &Access);
  return EFI_ERROR (Nor->IoStatus) ? Nor->IoStatus : Status;
}

// Program one protection unit. Keep the original lock state across erase,
// page programming and readback. A 1-to-0 change needs no erase.
EFI_STATUS
Eic7700SpiNorReplaceUnit (
  EIC7700_SPI_NOR *Nor OPTIONAL, UINT32 Offset, UINTN Size,
  CONST UINT8 *Before, CONST UINT8 *After
  )
{
  SPI_ACCESS Access;
  EFI_STATUS Status;
  BOOLEAN Relock, Erase = FALSE;
  UINT8 Verify[PAGE_BYTES];

  if (Nor == NULL) {
    Nor = &mLocal;
  }
  if (Before == NULL || After == NULL || (Size != SIZE_4KB && Size != SIZE_64KB) ||
      Offset % Size != 0 || Offset >= Nor->Size || Size > Nor->Size - Offset ||
      (Size == SIZE_64KB && (Offset == 0 || Offset == Nor->Size - Size))) {
    return EFI_INVALID_PARAMETER;
  }
  if (CompareMem (Before, After, Size) == 0) {
    return EFI_SUCCESS;
  }
  for (UINTN I = 0; I < Size; ++I) {
    if ((Before[I] & After[I]) != After[I]) {
      Erase = TRUE;
      break;
    }
  }
  Status = BeginAccess (Nor, &Access);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Unlock (Nor, Offset, &Relock);
  if (!EFI_ERROR (Status) && Erase) {
    Status = Execute (Nor, Size == SIZE_64KB ? SPI_FLASH_BE : SPI_FLASH_SE,
                      Size == SIZE_64KB ? EraseBlock : EraseSector, Offset, 0, NULL, NULL);
    if (!EFI_ERROR (Status)) {
      Status = WaitFlash (Nor);
    }
  }
  for (UINTN Pos = 0; Pos < Size && !EFI_ERROR (Status); Pos += PAGE_BYTES) {
    if (Erase) {
      UINTN Byte = 0;
      while (Byte < PAGE_BYTES && After[Pos + Byte] == 0xff) {
        ++Byte;
      }
      if (Byte == PAGE_BYTES) {
        continue;
      }
    } else if (CompareMem (Before + Pos, After + Pos, PAGE_BYTES) == 0) {
      continue;
    }
    Status = Execute (Nor, SPI_FLASH_PP, Program, Offset + (UINT32)Pos, PAGE_BYTES, After + Pos, NULL);
    if (!EFI_ERROR (Status)) {
      Status = WaitFlash (Nor);
    }
  }
  for (UINTN Pos = 0; Pos < Size && !EFI_ERROR (Status); Pos += PAGE_BYTES) {
    Status = Execute (Nor, SPI_FLASH_READ, ReadData, Offset + (UINT32)Pos, PAGE_BYTES, NULL, Verify);
    if (!EFI_ERROR (Status) && CompareMem (Verify, After + Pos, PAGE_BYTES) != 0) {
      Status = EFI_CRC_ERROR;
    }
  }
  if (Relock && !Nor->Faulted && !EFI_ERROR (Nor->IoStatus)) {
    EFI_STATUS LockStatus = BlockLock (Nor, Offset, TRUE);
    if (EFI_ERROR (LockStatus)) {
      Status = LockStatus;
    }
  }
  EndAccess (Nor, &Access);
  return EFI_ERROR (Nor->IoStatus) ? Nor->IoStatus : Status;
}

EFI_STATUS
Eic7700SpiNorReadUniqueId (UINT8 *Id, UINTN *Size)
{
  SPI_ACCESS Access;
  EFI_STATUS Status;
  EIC7700_SPI_NOR *Nor = &mLocal;
  UINT8 Bytes[1 + EIC7700_SPI_NOR_MAX_UID_SIZE];
  UINT8 Any = 0, All = 0xff;
  UINTN Length = Nor->Gd25Lq128 ? 16 : 8;

  if (Id == NULL || Size == NULL) { return EFI_INVALID_PARAMETER; }
  if (!Nor->Ready) { return EFI_NOT_READY; }
  if (*Size < Length) {
    *Size = Length;
    return EFI_BUFFER_TOO_SMALL;
  }
  Status = BeginAccess (Nor, &Access);
  if (EFI_ERROR (Status)) { return Status; }
  // 4Bh takes four bytes before the UID. The read framing sends three
  // zero address bytes. Discard the received byte from the dummy cycle.
  Status = Execute (Nor, 0x4b, ReadData, 0, 1 + Length, NULL, Bytes);
  EndAccess (Nor, &Access);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN I = 1; I <= Length; I++) { Any |= Bytes[I]; All &= Bytes[I]; }
  if (Any == 0 || All == 0xff) { return EFI_DEVICE_ERROR; }
  CopyMem (Id, Bytes + 1, Length);
  *Size = Length;
  return EFI_SUCCESS;
}

UINT32
Eic7700SpiNorSize (VOID)
{
  return mLocal.Ready ? mLocal.Size : 0;
}

EFI_STATUS
Eic7700SpiNorRead (UINT32 Offset, UINTN Size, VOID *Buffer)
{
  return Eic7700SpiNorReadDevice (&mLocal, Offset, Size, Buffer);
}

EFI_STATUS
Eic7700SpiNorWrite (UINT32 Offset, UINTN Size, CONST VOID *Buffer)
{
  return Eic7700SpiNorWriteDevice (&mLocal, Offset, Size, Buffer);
}

EFI_STATUS
Eic7700SpiNorErase (UINT32 Offset, UINTN Size)
{
  return Eic7700SpiNorEraseDevice (&mLocal, Offset, Size);
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
Eic7700SpiNorProbeDevice (EIC7700_SPI_NOR *Nor)
{
  EFI_STATUS  Status;
  SPI_ACCESS  Access;
  UINT8       Id[3];

  Nor->Ready = TRUE;
  Status = BeginAccess (Nor, &Access);
  if (!EFI_ERROR (Status)) {
    Status = Execute (Nor, SPI_FLASH_RDID, ReadJedecId, 0, sizeof (Id), NULL, Id);
    EndAccess (Nor, &Access);
    if (EFI_ERROR (Nor->IoStatus)) { Status = Nor->IoStatus; }
  }
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "EIC7700 Boot SPI ID: %02x %02x %02x\n", Id[0], Id[1], Id[2]));
  }
  if (!EFI_ERROR (Status)) {
    Nor->Gd25Lq128 = Id[0] == 0xc8 && Id[1] == 0x60 && Id[2] == 0x18;
    if (!Nor->Gd25Lq128 &&
        ((Id[0] != 0xef) || (Id[1] != 0x60) || ((Id[2] != 0x17) && (Id[2] != 0x18)))) {
      DEBUG ((DEBUG_ERROR, "Unsupported EIC7700 boot SPI ID: %02x %02x %02x\n", Id[0], Id[1], Id[2]));
      Status = EFI_UNSUPPORTED;
    }
  }
  Nor->Ready = !EFI_ERROR (Status);
  Nor->Size = Nor->Ready ? 1U << Id[2] : 0;
  return Status;
}

EFI_STATUS
Eic7700SpiNorInit (BOOLEAN Runtime)
{
  EFI_HOB_GUID_TYPE *Hob;
  CONST VOID *Fdt;
  EFI_STATUS Status;

  Hob = GetFirstGuidHob (&gFdtHobGuid);
  if (!Hob || GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (UINT64)) { return EFI_NOT_FOUND; }
  Fdt = (VOID *)(UINTN)ReadUnaligned64 (GET_GUID_HOB_DATA (Hob));
  if (!Fdt || FdtCheckHeader (Fdt)) { return EFI_COMPROMISED_DATA; }
  // D560 connects its host boot flash to the same controller on die 0.
  if ((FdtStringListSearch (Fdt, 0, "compatible", "eswin,eic7700") < 0) &&
      (FdtStringListSearch (Fdt, 0, "compatible", "eswin,eic7702-d560") < 0)) {
    return EFI_UNSUPPORTED;
  }
  Status = MapPage (mLocal.Spi, Runtime);
  if (!EFI_ERROR (Status)) { Status = MapPage (mLocal.Data, Runtime); }
  if (!EFI_ERROR (Status)) { Status = MapPage (mLocal.Clock, Runtime); }
  if (EFI_ERROR (Status)) { return Status; }
  Status = Eic7700SpiNorProbeDevice (&mLocal);
  if (!EFI_ERROR (Status) && Runtime && (mExitEvent == NULL)) {
    Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ExitBootServices,
                                 NULL, &gEfiEventExitBootServicesGuid, &mExitEvent);
  }
  mLocal.Ready = !EFI_ERROR (Status);
  return Status;
}

VOID
Eic7700SpiNorVirtualAddressChange (VOID)
{
  if (EFI_ERROR (gRT->ConvertPointer (0, (VOID **)&mLocal.Spi)) ||
      EFI_ERROR (gRT->ConvertPointer (0, (VOID **)&mLocal.Data)) ||
      EFI_ERROR (gRT->ConvertPointer (0, (VOID **)&mLocal.Clock)))
  {
    mLocal.Ready = FALSE;
  }
}
