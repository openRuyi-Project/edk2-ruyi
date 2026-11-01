/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>
#include <Pi/PiFirmwareVolume.h>
#include <Protocol/FirmwareVolumeBlock.h>

typedef enum {
  FirmwareUpdateRead,
  FirmwareUpdateErase,
  FirmwareUpdateWrite,
  FirmwareUpdateVerify
} FIRMWARE_UPDATE_PHASE;

typedef EFI_STATUS (*FIRMWARE_UPDATE_PROGRESS)(
  FIRMWARE_UPDATE_PHASE  Phase,
  UINT64                 Done,
  UINT64                 Total,
  VOID                   *Context
  );

EFI_STATUS
FirmwareUpdateFreezeNv (
  EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  **Fvb,
  EFI_FVB_ATTRIBUTES_2                *Attributes
  );

// Verify a file against its exact basename in a sha256sum-format list.
EFI_STATUS
FirmwareUpdateVerifyChecksum (
  CONST VOID    *Image,
  UINTN         Size,
  CONST CHAR8   *List,
  UINTN         ListSize,
  CONST CHAR16  *Name
  );
