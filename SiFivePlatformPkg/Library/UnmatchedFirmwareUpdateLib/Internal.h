/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>
#define UNMATCHED_FIRMWARE_SIZE  SIZE_32MB
#define UNMATCHED_CONFIG_OFFSET  0x10000U
#define UNMATCHED_CONFIG_SIZE    SIZE_64KB
#define UNMATCHED_FMAP_OFFSET    0x20000U
#define UNMATCHED_FMAP_SIZE      0x200U
#define UNMATCHED_CBFS_OFFSET    0x20200U
#define UNMATCHED_CBFS_SIZE      0x1edfe00U
#define UNMATCHED_NV_OFFSET      0x1f00000U
#define UNMATCHED_NV_SIZE        0x90000U
BOOLEAN
UnmatchedFirmwareMapValid (
  CONST UINT8  *Map
  );

EFI_STATUS
UnmatchedFirmwareImageValid (
  CONST VOID  *Buffer,
  UINTN       Size
  );
