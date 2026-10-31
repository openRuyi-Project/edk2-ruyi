/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Guid/FdtHob.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/Fu740SpiNorLib.h>

#define SPI_SCKDIV       0x00
#define SPI_SCKMODE      0x04
#define SPI_CSID         0x10
#define SPI_CSDEF        0x14
#define SPI_CSMODE       0x18
#define SPI_FMT          0x40
#define SPI_TXDATA       0x48
#define SPI_RXDATA       0x4c
#define SPI_FCTRL        0x60
#define SPI_CSMODE_AUTO  0
#define SPI_CSMODE_HOLD  2
#define SPI_FMT_RX8      (8U << 16)
#define FLASH_SIZE       SIZE_32MB

STATIC UINTN         mSpi;
STATIC UINT32        mDivider;
STATIC BOOLEAN       mReady;
STATIC CONST UINT32  mRegisters[] = {
  SPI_SCKDIV, SPI_SCKMODE, SPI_CSID, SPI_CSDEF, SPI_CSMODE, SPI_FMT, SPI_FCTRL
};

STATIC EFI_STATUS
MapRegisters (
  UINTN  Base
  )
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;
  EFI_STATUS                       Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Base, SIZE_4KB, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);

  if (EFI_ERROR (Status)) {
    Status = gDS->GetMemorySpaceDescriptor (Base, &Descriptor);
    if (EFI_ERROR (Status) || (Descriptor.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) ||
        (Descriptor.BaseAddress > Base) || (Descriptor.Length < SIZE_4KB) ||
        (Base - Descriptor.BaseAddress > Descriptor.Length - SIZE_4KB))
    {
      return EFI_UNSUPPORTED;
    }
  }

  Status = gDS->GetMemorySpaceDescriptor (Base, &Descriptor);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // A boot-time settings instance must not remove the runtime NV mapping.
  return gDS->SetMemorySpaceAttributes (
                Base,
                SIZE_4KB,
                EFI_MEMORY_UC | (Descriptor.Attributes & EFI_MEMORY_RUNTIME)
                );
}

STATIC EFI_STATUS
Byte (
  UINT8  Output,
  UINT8  *Input
  )
{
  UINT32  Value;
  UINTN   Retry;

  for (Retry = 0; Retry < 10000; Retry++) {
    if (!(MmioRead32 (mSpi + SPI_TXDATA) & BIT31)) {
      break;
    }

    MicroSecondDelay (1);
  }

  if (Retry == 10000) {
    return EFI_TIMEOUT;
  }

  MmioWrite32 (mSpi + SPI_TXDATA, Output);
  for (Retry = 0; Retry < 10000; Retry++) {
    Value = MmioRead32 (mSpi + SPI_RXDATA);
    if (!(Value & BIT31)) {
      if (Input) {
        *Input = (UINT8)Value;
      }

      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
Transaction (
  CONST UINT8  *Command,
  UINTN        CommandSize,
  CONST UINT8  *Output,
  UINT8        *Input,
  UINTN        Size
  )
{
  UINT32      Saved[ARRAY_SIZE (mRegisters)];
  EFI_STATUS  Status = EFI_SUCCESS;
  UINTN       Index;

  for (Index = 0; Index < ARRAY_SIZE (mRegisters); Index++) {
    Saved[Index] = MmioRead32 (mSpi + mRegisters[Index]);
  }

  MmioWrite32 (mSpi + SPI_FCTRL, 0);
  MmioWrite32 (mSpi + SPI_CSMODE, SPI_CSMODE_AUTO);
  MmioWrite32 (mSpi + SPI_SCKDIV, mDivider);
  MmioWrite32 (mSpi + SPI_SCKMODE, 0);
  MmioWrite32 (mSpi + SPI_CSID, 0);
  MmioWrite32 (mSpi + SPI_CSDEF, Saved[3] | BIT0);
  MmioWrite32 (mSpi + SPI_FMT, SPI_FMT_RX8);
  // Discard any bytes left by the previous FIFO user before asserting CS.
  for (Index = 0; Index < 32; Index++) {
    if (MmioRead32 (mSpi + SPI_RXDATA) & BIT31) {
      break;
    }
  }

  if (Index == 32) {
    Status = EFI_DEVICE_ERROR;
  } else {
    MmioWrite32 (mSpi + SPI_CSMODE, SPI_CSMODE_HOLD);
    for (Index = 0; Index < CommandSize && !EFI_ERROR (Status); Index++) {
      Status = Byte (Command[Index], NULL);
    }

    for (Index = 0; Index < Size && !EFI_ERROR (Status); Index++) {
      Status = Byte (Output ? Output[Index] : 0xff, Input ? Input + Index : NULL);
    }
  }

  MmioWrite32 (mSpi + SPI_CSMODE, SPI_CSMODE_AUTO);
  // Restore the memory-mapped read setup last. No flash address-mode change.
  for (Index = 0; Index < ARRAY_SIZE (mRegisters); Index++) {
    MmioWrite32 (mSpi + mRegisters[Index], Saved[Index]);
  }

  return Status;
}

STATIC EFI_STATUS
StatusRegister (
  UINT8  *Value
  )
{
  CONST UINT8  Command = 0x05;

  return Transaction (&Command, 1, NULL, Value, 1);
}

STATIC EFI_STATUS
Ready (
  UINTN  TimeoutUs
  )
{
  EFI_STATUS  Status;
  UINT8       Value;

  for (UINTN Elapsed = 0; Elapsed < TimeoutUs; Elapsed += 10) {
    Status = StatusRegister (&Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (!(Value & BIT0)) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
WriteEnable (
  VOID
  )
{
  CONST UINT8  Command = 0x06;
  UINT8        Value;
  EFI_STATUS   Status = Ready (3000000);

  if (!EFI_ERROR (Status)) {
    Status = Transaction (&Command, 1, NULL, NULL, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = StatusRegister (&Value);
  }

  if (!EFI_ERROR (Status) && !(Value & BIT1)) {
    Status = EFI_WRITE_PROTECTED;
  }

  return Status;
}

STATIC VOID
AddressCommand (
  UINT8   *Command,
  UINT8   Opcode,
  UINT32  Offset
  )
{
  Command[0] = Opcode;
  Command[1] = (UINT8)(Offset >> 24);
  Command[2] = (UINT8)(Offset >> 16);
  Command[3] = (UINT8)(Offset >> 8);
  Command[4] = (UINT8)Offset;
}

EFI_STATUS
Fu740SpiNorRead (
  UINT32  Offset,
  UINTN   Size,
  VOID    *Buffer
  )
{
  UINT8  Command[5];
  EFI_STATUS Status;

  if (!mReady) {
    return EFI_NOT_READY;
  }

  if (!Buffer || (Offset >= FLASH_SIZE) || (Size > FLASH_SIZE - Offset)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = Ready (3000000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  AddressCommand (Command, 0x13, Offset);
  return Transaction (Command, sizeof (Command), NULL, Buffer, Size);
}

EFI_STATUS
Fu740SpiNorWrite (
  UINT32      Offset,
  UINTN       Size,
  CONST VOID  *Buffer
  )
{
  UINT8       Command[5];
  EFI_STATUS  Status;

  if (!mReady) {
    return EFI_NOT_READY;
  }

  if (!Buffer || !Size || ((Offset >= FLASH_SIZE) || (Size > FLASH_SIZE - Offset)) || (Size > 256 - (Offset & 255))) {
    return EFI_INVALID_PARAMETER;
  }

  AddressCommand (Command, 0x12, Offset);
  Status = WriteEnable ();
  if (!EFI_ERROR (Status)) {
    Status = Transaction (Command, sizeof (Command), Buffer, NULL, Size);
  }

  if (!EFI_ERROR (Status)) {
    Status = Ready (100000);
  }

  return Status;
}

EFI_STATUS
Fu740SpiNorErase (
  UINT32  Offset
  )
{
  UINT8       Command[5];
  EFI_STATUS  Status;

  if (!mReady) {
    return EFI_NOT_READY;
  }

  if ((Offset >= FLASH_SIZE) || (Offset % SIZE_4KB)) {
    return EFI_INVALID_PARAMETER;
  }

  AddressCommand (Command, 0x21, Offset);
  Status = WriteEnable ();
  if (!EFI_ERROR (Status)) {
    Status = Transaction (Command, sizeof (Command), NULL, NULL, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = Ready (3000000);
  }

  return Status;
}

VOID
Fu740SpiNorVirtualAddressChange (
  VOID
  )
{
  if (EFI_ERROR (gRT->ConvertPointer (0, (VOID **)&mSpi))) {
    mReady = FALSE;
  }
}

EFI_STATUS
Fu740SpiNorInit (
  BOOLEAN  Runtime
  )
{
  EFI_HOB_GUID_TYPE  *Hob = GetFirstGuidHob (&gFdtHobGuid);
  CONST VOID         *Fdt;
  CONST UINT32       *Data;
  INT32              Node, Clock, Reference, Length;
  UINTN              Prci;
  UINT32             RefHz, Config, Divider;
  UINT64             Rate;
  EFI_STATUS         Status;
  CONST UINT8        Command = 0x9f;
  UINT8              Id[3];

  if (!Hob || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (UINT64))) {
    return EFI_NOT_FOUND;
  }

  Fdt = (VOID *)(UINTN)ReadUnaligned64 (GET_GUID_HOB_DATA (Hob));
  if (FdtCheckHeader (Fdt)) {
    return EFI_COMPROMISED_DATA;
  }

  Node = FdtPathOffset (Fdt, "/soc/spi@10040000");
  if ((Node < 0) || (FdtStringListSearch (Fdt, Node, "compatible", "sifive,fu740-c000-spi") < 0)) {
    return EFI_UNSUPPORTED;
  }

  Data = FdtGetProp (Fdt, Node, "reg", &Length);
  if (!Data || (Length < 16) || Data[0] || (Fdt32ToCpu (Data[1]) != 0x10040000) ||
      Data[2] || (Fdt32ToCpu (Data[3]) < SIZE_4KB))
  {
    return EFI_COMPROMISED_DATA;
  }

  mSpi = Fdt32ToCpu (Data[1]);
  Data = FdtGetProp (Fdt, Node, "clocks", &Length);
  if (!Data || (Length != 8) || (Fdt32ToCpu (Data[1]) != 7)) {
    return EFI_UNSUPPORTED;
  }

  Clock = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Data[0]));
  if ((Clock < 0) || (FdtStringListSearch (Fdt, Clock, "compatible", "sifive,fu740-c000-prci") < 0)) {
    return EFI_UNSUPPORTED;
  }

  Data = FdtGetProp (Fdt, Clock, "reg", &Length);
  if (!Data || (Length != 16) || Data[0] || (Fdt32ToCpu (Data[1]) != 0x10000000) ||
      Data[2] || (Fdt32ToCpu (Data[3]) < SIZE_4KB))
  {
    return EFI_COMPROMISED_DATA;
  }

  Prci = Fdt32ToCpu (Data[1]);
  Data = FdtGetProp (Fdt, Clock, "clocks", &Length);
  if (!Data || (Length < 4)) {
    return EFI_COMPROMISED_DATA;
  }

  Reference = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Data[0]));
  if (Reference < 0) {
    return EFI_COMPROMISED_DATA;
  }

  Data = FdtGetProp (Fdt, Reference, "clock-frequency", &Length);
  if (!Data || (Length != 4) || !*Data) {
    return EFI_COMPROMISED_DATA;
  }

  RefHz  = Fdt32ToCpu (*Data);
  Status = MapRegisters (Prci);
  if (!EFI_ERROR (Status)) {
    Status = MapRegisters (mSpi);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Rate   = RefHz;
  Config = MmioRead32 (Prci + 0x50);
  if (!(MmioRead32 (Prci + 0x58) & BIT0) && !(Config & BIT24)) {
    Rate = DivU64x32 (
             MultU64x32 (RefHz, 2 * (((Config >> 6) & 0x1ff) + 1)),
             ((Config & 0x3f) + 1) * (1U << ((Config >> 15) & 7))
             );
  }

  Divider = MmioRead32 (Prci + 0x5c);
  if (Divider > MAX_UINT32 - 2) {
    return EFI_DEVICE_ERROR;
  }

  Rate = DivU64x32 (Rate, Divider + 2);
  if (!Rate) {
    return EFI_DEVICE_ERROR;
  }

  // Keep the serial clock at or below 25 MHz for the board's SPI NOR.
  mDivider = (UINT32)DivU64x32 (Rate + 50000000 - 1, 50000000) - 1;
  Status   = Transaction (&Command, 1, NULL, Id, sizeof (Id));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // IS25WP256D: 32 MiB, READ4/PP4/4 KiB SE4 opcodes 13h/12h/21h.
  if ((Id[0] != 0x9d) || (Id[1] != 0x70) || (Id[2] != 0x19)) {
    return EFI_UNSUPPORTED;
  }

  Status = Ready (3000000);
  if (!EFI_ERROR (Status) && Runtime) {
    Status = gDS->SetMemorySpaceAttributes (mSpi, SIZE_4KB, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
  }

  if (!EFI_ERROR (Status)) {
    mReady = TRUE;
  }

  return Status;
}
