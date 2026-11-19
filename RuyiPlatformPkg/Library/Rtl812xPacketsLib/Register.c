/* SPDX-License-Identifier: GPL-2.0-only */
#include <Library/Rtl812xHwLib.h>
#include <Library/TimerLib.h>
EFI_STATUS
RtlRead (
  EFI_PCI_IO_PROTOCOL        *Pci,
  EFI_PCI_IO_PROTOCOL_WIDTH  Width,
  UINT32                     Register,
  VOID                       *Value
  )
{
  return Pci->Mem.Read (Pci, Width, 2, Register, 1, Value);
}

EFI_STATUS
RtlWrite (
  EFI_PCI_IO_PROTOCOL        *Pci,
  EFI_PCI_IO_PROTOCOL_WIDTH  Width,
  UINT32                     Register,
  UINT32                     Value
  )
{
  return Pci->Mem.Write (Pci, Width, 2, Register, 1, &Value);
}

EFI_STATUS
RtlReset (
  EFI_PCI_IO_PROTOCOL  *Pci
  )
{
  EFI_STATUS  Status = RtlWrite (Pci, EfiPciIoWidthUint8, 0x37, BIT4);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (UINTN Us = 0; Us < 10000; Us += 100) {
    UINT8  Command;

    Status = RtlRead (Pci, EfiPciIoWidthUint8, 0x37, &Command);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (!(Command & (BIT4 | BIT3 | BIT2))) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (100);
  }

  return EFI_TIMEOUT;
}
