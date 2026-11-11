/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Library/FirmwareUpdateLib.h>

typedef struct {
  UINTN           ImageSize;
  CONST CHAR16    *PreservationNotice;
} PLATFORM_FIRMWARE_UPDATE_INFO;

VOID
PlatformFirmwareUpdateGetInfo (
  PLATFORM_FIRMWARE_UPDATE_INFO  *Info
  );

// Check the board/image format and the installed flash layout. The caller
// must also check SHA256SUMS for updates selected from a filesystem.
EFI_STATUS
PlatformFirmwareUpdateValidate (
  CONST VOID  *Image,
  UINTN       Size
  );

EFI_STATUS
PlatformFirmwareUpdate (
  CONST VOID                *Image,
  UINTN                     Size,
  FIRMWARE_UPDATE_PROGRESS  Progress OPTIONAL,
  VOID                      *Context OPTIONAL
  );
