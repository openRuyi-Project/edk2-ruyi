/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>

BOOLEAN
FdtNodeEnabled (CONST VOID *Fdt, INT32 Node);

// Read an MMIO resource and translate it through DT buses with 64-bit cells.
EFI_STATUS
FdtGetMmio (CONST VOID *Fdt, INT32 Node, UINTN Index, UINTN Minimum,
            UINTN *Base, UINTN *Bytes OPTIONAL);

EFI_STATUS
FdtMapMmio (UINTN Base, UINTN Bytes);
