/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_NV_FLASH_H
#define K3_NV_FLASH_H
#include <Library/K3NvFvbLib.h>

EFI_STATUS
K3NvFlashInitialize (
  UINT32           FlashSize,
  K3_NV_FLASH_OPS  *Ops
  );

VOID
K3NvFlashVirtualAddressChange (
  VOID
  );

#endif
