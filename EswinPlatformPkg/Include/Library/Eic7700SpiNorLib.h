// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

EFI_STATUS Eic7700SpiNorInit (BOOLEAN Runtime);
UINT32 Eic7700SpiNorSize (VOID);
EFI_STATUS Eic7700SpiNorRead (UINT32 Offset, UINTN Size, VOID *Buffer);
// Program one page without crossing its 256-byte boundary.
EFI_STATUS Eic7700SpiNorWrite (UINT32 Offset, UINTN Size, CONST VOID *Buffer);
// Erase one aligned 4 KiB sector or 64 KiB block.
EFI_STATUS Eic7700SpiNorErase (UINT32 Offset, UINTN Size);
VOID Eic7700SpiNorVirtualAddressChange (VOID);
