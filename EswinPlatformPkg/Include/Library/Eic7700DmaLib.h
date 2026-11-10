/** @file
  Address-limited mappings provided by the EIC7700 DmaLib instance.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef EIC7700_DMA_LIB_H
#define EIC7700_DMA_LIB_H

#include <Library/DmaLib.h>

/**
  Map a buffer whose device address must not exceed MaximumAddress.
  Streaming mappings use a bounce buffer when required. Common buffers
  outside the address limit return EFI_UNSUPPORTED. Release mappings with
  DmaUnmap().
**/
EFI_STATUS EFIAPI
Eic7700DmaMap (
  DMA_MAP_OPERATION     Operation,
  VOID                  *Host,
  UINTN                 *Bytes,
  PHYSICAL_ADDRESS      *Device,
  VOID                  **Mapping,
  EFI_PHYSICAL_ADDRESS  MaximumAddress
  );

/**
  Allocate an uncached common buffer backed by RAM below MaximumAddress.
  The limit applies to its device address, not the CPU alias. Release it
  with DmaFreeBuffer().
**/
EFI_STATUS EFIAPI
Eic7700DmaAllocateBuffer (
  EFI_MEMORY_TYPE       Type,
  UINTN                 Pages,
  VOID                  **Host,
  EFI_PHYSICAL_ADDRESS  MaximumAddress
  );

#endif
