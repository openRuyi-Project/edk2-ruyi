/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef NV_FLASH_H
#define NV_FLASH_H
#include <Library/NvFvbLib.h>

#define K3_NV_FLASH_OFFSET  0x700000U

EFI_STATUS
K3NvFlashInitialize (
  UINT32           FlashSize,
  NV_FLASH_OPS  *Ops
  );

VOID
K3NvFlashVirtualAddressChange (
  VOID
  );

#endif
