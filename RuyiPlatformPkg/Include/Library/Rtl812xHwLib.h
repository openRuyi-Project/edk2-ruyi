/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef RTL812X_HW_LIB_H
#define RTL812X_HW_LIB_H
#include <Uefi.h>
#include <Protocol/PciIo.h>

typedef struct {
  UINT32 PciId;
  UINT32 Revision;
  UINT32 BarSize;
  EFI_PCI_IO_PROTOCOL_WIDTH DoorbellWidth;
} RTL_CHIP;

extern CONST RTL_CHIP gRtlChip;
EFI_STATUS RtlReadStationAddress (EFI_HANDLE Controller, EFI_PCI_IO_PROTOCOL *Pci, BOOLEAN Create, UINT8 Mac[6]);
EFI_STATUS RtlSetRxTxConfig (EFI_PCI_IO_PROTOCOL *Pci);

EFI_STATUS
RtlRead (
  EFI_PCI_IO_PROTOCOL        *Pci,
  EFI_PCI_IO_PROTOCOL_WIDTH  Width,
  UINT32                     Register,
  VOID                       *Value
  );

EFI_STATUS
RtlWrite (
  EFI_PCI_IO_PROTOCOL        *Pci,
  EFI_PCI_IO_PROTOCOL_WIDTH  Width,
  UINT32                     Register,
  UINT32                     Value
  );

EFI_STATUS
RtlReset (
  EFI_PCI_IO_PROTOCOL  *Pci
  );

EFI_STATUS
RtlPrepareMac (
  EFI_PCI_IO_PROTOCOL  *Pci,
  CONST UINT8          Mac[6]
  );

EFI_STATUS
RtlDrain (
  EFI_PCI_IO_PROTOCOL  *Pci
  );

#endif
