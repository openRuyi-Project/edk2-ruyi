/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

typedef struct {
  EFI_BLOCK_IO_PROTOCOL       *Io;
  EFI_DEVICE_PATH_PROTOCOL    *Path;
  UINT64                      Bytes;
  UINT8                       Lun;
  BOOLEAN                     WriteProtected;
} UFS_USER_MEDIA;

// Return the unique enabled non-boot user LUN. Protocol pointers are borrowed.
// Multiple distinct user LUNs are ambiguous and return EFI_UNSUPPORTED.
EFI_STATUS
UfsFindUserMedia (
  OUT UFS_USER_MEDIA  *Media
  );
