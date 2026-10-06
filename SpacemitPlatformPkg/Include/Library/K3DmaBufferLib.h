/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_DMA_BUFFER_LIB_H
#define K3_DMA_BUFFER_LIB_H
#include <Uefi.h>

/* Platform extension to the Spacemit DmaLib instance. Maximum is inclusive.
   The entire allocation remains in normal DRAM at/above 4 GiB. No DMA32
   pool or global allocation limit is created. Free/map use standard DmaLib.
   The same verified PBMT/cache policy as ordinary common buffers applies. */
EFI_STATUS EFIAPI
K3DmaAllocateBufferBelow (
  EFI_MEMORY_TYPE       Type,
  UINTN                 Pages,
  EFI_PHYSICAL_ADDRESS  Maximum,
  VOID                  **Host
  );

#endif
