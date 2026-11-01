/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef FU740_SPI_NOR_LIB_H_
#define FU740_SPI_NOR_LIB_H_
#include <Uefi.h>
EFI_STATUS
Fu740SpiNorInit (
  BOOLEAN  Runtime
  );

EFI_STATUS
Fu740SpiNorRead (
  UINT32  Offset,
  UINTN   Size,
  VOID    *Buffer
  );

/* One page at a time; a program operation may not cross a 256-byte boundary. */
EFI_STATUS
Fu740SpiNorWrite (
  UINT32      Offset,
  UINTN       Size,
  CONST VOID  *Buffer
  );

EFI_STATUS
Fu740SpiNorErase (
  UINT32  Offset
  );

VOID
Fu740SpiNorVirtualAddressChange (
  VOID
  );

// Erase one aligned 4 KiB sector or 64 KiB block.
EFI_STATUS
Fu740SpiNorEraseBlock (
  UINT32  Offset,
  UINTN   Size
  );

#endif
