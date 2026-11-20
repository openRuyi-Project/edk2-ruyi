/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RTL8125B register settings from U-Boot drivers/net/rtl8169.c. */
#include <Library/Rtl812xHwLib.h>
#include <Library/Sg2042MacLib.h>

// RTL8125B, XID 641. Later revisions use a different descriptor setup.
CONST RTL_CHIP gRtlChip = { 0x812510ec, 0x64100000, 0x100, EfiPciIoWidthUint8 };

EFI_STATUS
RtlReadStationAddress (EFI_HANDLE Controller, EFI_PCI_IO_PROTOCOL *Pci, BOOLEAN Create, UINT8 Mac[6])
{
  return Sg2042PciMac (Controller, Pci, Create, Mac, NULL);
}

EFI_STATUS
RtlPrepareMac (EFI_PCI_IO_PROTOCOL *Pci, CONST UINT8 Mac[6])
{
  EFI_STATUS Status = RtlReset (Pci);
  if (EFI_ERROR (Status)) { return Status; }
  Status = RtlWrite (Pci, EfiPciIoWidthUint8, 0x50, 0xc0);
  if (EFI_ERROR (Status)) { return Status; }
  Status = RtlWrite (Pci, EfiPciIoWidthUint8, 0xec, 0x3f);
  if (!EFI_ERROR (Status)) {
    Status = Pci->Mem.Write (Pci, EfiPciIoWidthUint8, 2, 0, 6, (VOID *)Mac);
  }
  EFI_STATUS Lock = RtlWrite (Pci, EfiPciIoWidthUint8, 0x50, 0);
  return EFI_ERROR (Status) ? Status : Lock;
}

EFI_STATUS
RtlSetRxTxConfig (EFI_PCI_IO_PROTOCOL *Pci)
{
  UINT32 Rx;
  EFI_STATUS Status = RtlRead (Pci, EfiPciIoWidthUint32, 0x44, &Rx);
  if (!EFI_ERROR (Status)) {
    Status = RtlWrite (Pci, EfiPciIoWidthUint32, 0x44,
                       (Rx & 0xff7e1880) | (7U << 13) | (6U << 8) | BIT3 | BIT1);
  }
  return EFI_ERROR (Status) ? Status :
         RtlWrite (Pci, EfiPciIoWidthUint32, 0x40, (3U << 24) | (6U << 8));
}

EFI_STATUS
RtlDrain (EFI_PCI_IO_PROTOCOL *Pci)
{
  EFI_STATUS Status = RtlWrite (Pci, EfiPciIoWidthUint32, 0x38, 0);
  return EFI_ERROR (Status) ? Status : RtlReset (Pci);
}
