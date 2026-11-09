// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

#define EIC7700_XHCI_SIZE  0xc100

// The caller must stop and disconnect the current controller driver first.
EFI_STATUS
Eic7700UsbInitialize (
  CONST VOID  *Fdt,
  INT32       Node,
  BOOLEAN     Device,
  UINTN       *Core
  );
