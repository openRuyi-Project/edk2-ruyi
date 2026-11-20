/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>
#include <Protocol/PciIo.h>

BOOLEAN
Sg2042ValidMac (CONST UINT8 Mac[6]);

EFI_STATUS
Sg2042FallbackMac (
  CONST VOID *Port, UINTN PortSize, BOOLEAN Create, UINT8 Mac[6]
  );

EFI_STATUS
Sg2042PciMac (
  EFI_HANDLE Controller, EFI_PCI_IO_PROTOCOL *Pci, BOOLEAN Create,
  UINT8 Mac[6], BOOLEAN *NeedsHandoff OPTIONAL
  );
