/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef UNMATCHED_NV_FLASH_H_
#define UNMATCHED_NV_FLASH_H_
#include <Library/NvFvbLib.h>
#define UNMATCHED_NV_FLASH_OFFSET  0x1f00000U
EFI_STATUS
UnmatchedNvFlashInitialize (
  NV_FLASH_OPS  *Ops
  );

#endif
