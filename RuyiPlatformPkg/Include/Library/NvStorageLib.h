/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Library/NvFvbLib.h>

typedef VOID (*NV_FLASH_VIRTUAL_CHANGE)(VOID);

// Call from a runtime driver after reserving the flash controller for UEFI.
// Flash callbacks and Context must remain valid at runtime.
EFI_STATUS
NvStorageInstall (
  CONST NV_FLASH_OPS       *Flash,
  UINT32                   FlashOffset,
  NV_FLASH_VIRTUAL_CHANGE   ConvertFlash
  );
