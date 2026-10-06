/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef RTL8127_PACKETS_INTERNAL_H
#define RTL8127_PACKETS_INTERNAL_H
#include <Library/Rtl8127PacketsLib.h>

EFI_STATUS
R27Read (
  EFI_PCI_IO_PROTOCOL        *Pci,
  EFI_PCI_IO_PROTOCOL_WIDTH  Width,
  UINT32                     Register,
  VOID                       *Value
  );

EFI_STATUS
R27Write (
  EFI_PCI_IO_PROTOCOL        *Pci,
  EFI_PCI_IO_PROTOCOL_WIDTH  Width,
  UINT32                     Register,
  UINT32                     Value
  );

EFI_STATUS
R27Reset (
  EFI_PCI_IO_PROTOCOL  *Pci
  );

EFI_STATUS
R27PrepareMac (
  EFI_PCI_IO_PROTOCOL  *Pci,
  CONST UINT8          Mac[6]
  );

EFI_STATUS
R27Drain (
  EFI_PCI_IO_PROTOCOL  *Pci
  );

#endif
