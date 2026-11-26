// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

EFI_STATUS Eic7700SpiNorInit (BOOLEAN Runtime);
UINT32 Eic7700SpiNorSize (VOID);
#define EIC7700_SPI_NOR_MAX_UID_SIZE  16
// Size is the caller's buffer capacity on input and the UID length on output.
EFI_STATUS Eic7700SpiNorReadUniqueId (UINT8 *Id, UINTN *Size);
EFI_STATUS Eic7700SpiNorRead (UINT32 Offset, UINTN Size, VOID *Buffer);
// Program one page without crossing its 256-byte boundary.
EFI_STATUS Eic7700SpiNorWrite (UINT32 Offset, UINTN Size, CONST VOID *Buffer);
// Erase one aligned 4 KiB sector or 64 KiB block.
EFI_STATUS Eic7700SpiNorErase (UINT32 Offset, UINTN Size);
VOID Eic7700SpiNorVirtualAddressChange (VOID);

// Boot-time access to a second controller. Transfer uses aligned 32-bit words.
typedef EFI_STATUS (*EIC7700_SPI_TRANSFER)(
  VOID *Context, BOOLEAN Write, UINTN Address, UINT32 *Words, UINTN Count
  );

typedef struct {
  UINTN                 Spi;
  UINTN                 Data;
  UINTN                 Clock;
  EIC7700_SPI_TRANSFER  Transfer;
  VOID                  *Context;
  EFI_STATUS            IoStatus;
  UINT32                Size;
  BOOLEAN               Gd25Lq128;
  BOOLEAN               Ready;
  BOOLEAN               Faulted;
  BOOLEAN               Busy;
} EIC7700_SPI_NOR;

EFI_STATUS Eic7700SpiNorProbeDevice (EIC7700_SPI_NOR *Nor);
EFI_STATUS Eic7700SpiNorReadDevice (EIC7700_SPI_NOR *Nor, UINT32 Offset, UINTN Size, VOID *Buffer);
EFI_STATUS Eic7700SpiNorWriteDevice (EIC7700_SPI_NOR *Nor, UINT32 Offset, UINTN Size, CONST VOID *Buffer);
EFI_STATUS Eic7700SpiNorEraseDevice (EIC7700_SPI_NOR *Nor, UINT32 Offset, UINTN Size);

// Replace and verify an aligned 4 KiB sector or 64 KiB block, using its
// previously read contents. A NULL controller selects the local boot NOR.
EFI_STATUS
Eic7700SpiNorReplaceUnit (
  EIC7700_SPI_NOR *Nor OPTIONAL, UINT32 Offset, UINTN Size,
  CONST UINT8 *Before, CONST UINT8 *After
  );
