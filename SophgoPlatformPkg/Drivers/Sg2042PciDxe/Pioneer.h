/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>

INT32
Sg2042PciPortIndex (IN UINTN Base);

EFI_STATUS
Sg2042PreparePciFdt (OUT VOID **Fdt);
