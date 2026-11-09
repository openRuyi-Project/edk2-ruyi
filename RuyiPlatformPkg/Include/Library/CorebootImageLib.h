/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>

// Bounds are provided by the platform's validated FMAP/container.
// The caller must register the LZMA guided-section handler.
typedef struct {
  CONST CHAR8 *Compatible;
  CONST CHAR8 *BoardConfig;
  CONST CHAR8 *RamConfig;  // Optional key whose only accepted value is n.
  UINT64       PayloadAddress;
  UINT32       PayloadBytes;
} COREBOOT_IMAGE_LAYOUT;

EFI_STATUS
CorebootImageValidate (
  CONST VOID                 *Image,
  UINTN                       Size,
  CONST COREBOOT_IMAGE_LAYOUT  *Layout
  );
