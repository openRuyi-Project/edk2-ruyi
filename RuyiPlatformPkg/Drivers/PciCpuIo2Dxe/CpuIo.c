/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/CpuIo2.h>
#include <Protocol/PlatformPciController.h>

STATIC RUYI_PCI_CONTROLLER_PROTOCOL  *mController;

STATIC EFI_STATUS
Transfer (
  EFI_CPU_IO_PROTOCOL_WIDTH  Width,
  UINT64                     Address,
  UINTN                      Count,
  VOID                       *Buffer,
  BOOLEAN                    Write,
  BOOLEAN                    Io
  )
{
  UINTN   Bytes, AddressStride, BufferStride, Span;
  UINT64  Value, Base, Limit;
  UINT8   *Pointer = Buffer;

  if (((UINT32)Width >= EfiCpuIoWidthMaximum) || (Count && !Buffer)) {
    return EFI_INVALID_PARAMETER;
  }

  Bytes = 1U << (Width & 3);
  if (Io && (Bytes == 8)) {
    return EFI_UNSUPPORTED;
  }

  if (Address & (Bytes - 1)) {
    return EFI_UNSUPPORTED;
  }

  if (!Count) {
    return EFI_SUCCESS;
  }

  AddressStride = Width >= EfiCpuIoWidthFifoUint8 && Width <= EfiCpuIoWidthFifoUint64 ? 0 : Bytes;
  BufferStride  = Width >= EfiCpuIoWidthFillUint8 ? 0 : Bytes;
  if (Count > MAX_UINTN / Bytes) {
    return EFI_INVALID_PARAMETER;
  }

  Span = AddressStride ? Count * Bytes : Bytes;
  if (Address > MAX_UINT64 - (Span - 1)) {
    return EFI_UNSUPPORTED;
  }

  if ((UINTN)Buffer > MAX_UINTN - (BufferStride ? Count * Bytes - 1 : Bytes - 1)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Io) {
    Base  = mController->RootBridge.Io.Base - mController->RootBridge.Io.Translation;
    Limit = mController->RootBridge.Io.Limit - mController->RootBridge.Io.Translation;
    if ((Base > Limit) || (Address < Base) || (Address + Span - 1 > Limit)) {
      return EFI_UNSUPPORTED;
    }
  }

  while (Count--) {
    if (Write) {
      Value = 0;
      CopyMem (&Value, Pointer, Bytes);
      switch (Bytes) {
        case 1: MmioWrite8 (Address, Value);
          break;
        case 2: MmioWrite16 (Address, Value);
          break;
        case 4: MmioWrite32 (Address, Value);
          break;
        case 8: MmioWrite64 (Address, Value);
          break;
      }
    } else {
      switch (Bytes) {
        case 1: Value = MmioRead8 (Address);
          break;
        case 2: Value = MmioRead16 (Address);
          break;
        case 4: Value = MmioRead32 (Address);
          break;
        default: Value = MmioRead64 (Address);
          break;
      }

      CopyMem (Pointer, &Value, Bytes);
    }

    Address += AddressStride;
    Pointer += BufferStride;
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MemoryRead (
  EFI_CPU_IO2_PROTOCOL       *This,
  EFI_CPU_IO_PROTOCOL_WIDTH  Width,
  UINT64                     Address,
  UINTN                      Count,
  VOID                       *Buffer
  )
{
  return Transfer (Width, Address, Count, Buffer, FALSE, FALSE);
}

STATIC EFI_STATUS EFIAPI
MemoryWrite (
  EFI_CPU_IO2_PROTOCOL       *This,
  EFI_CPU_IO_PROTOCOL_WIDTH  Width,
  UINT64                     Address,
  UINTN                      Count,
  VOID                       *Buffer
  )
{
  return Transfer (Width, Address, Count, Buffer, TRUE, FALSE);
}

STATIC EFI_STATUS EFIAPI
IoRead (
  EFI_CPU_IO2_PROTOCOL       *This,
  EFI_CPU_IO_PROTOCOL_WIDTH  Width,
  UINT64                     Address,
  UINTN                      Count,
  VOID                       *Buffer
  )
{
  return Transfer (Width, Address, Count, Buffer, FALSE, TRUE);
}

STATIC EFI_STATUS EFIAPI
IoWrite (
  EFI_CPU_IO2_PROTOCOL       *This,
  EFI_CPU_IO_PROTOCOL_WIDTH  Width,
  UINT64                     Address,
  UINTN                      Count,
  VOID                       *Buffer
  )
{
  return Transfer (Width, Address, Count, Buffer, TRUE, TRUE);
}

EFI_STATUS EFIAPI
PciCpuIoEntry (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                   Status;
  EFI_HANDLE                   Handle   = NULL;
  STATIC EFI_CPU_IO2_PROTOCOL  Protocol = {
    { MemoryRead, MemoryWrite }, { IoRead, IoWrite }
  };

  Status = gBS->LocateProtocol (&gRuyiPciControllerProtocolGuid, NULL, (VOID **)&mController);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return gBS->InstallProtocolInterface (&Handle, &gEfiCpuIo2ProtocolGuid, EFI_NATIVE_INTERFACE, &Protocol);
}
