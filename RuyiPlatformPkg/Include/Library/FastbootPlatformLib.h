// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Library/FastbootStorageLib.h>

EFI_STATUS
FastbootPlatformInit (
  VOID
  );

VOID
FastbootPlatformStop (
  VOID
  );

EFI_STATUS
FastbootOem (
  CONST CHAR8  *Command,
  CONST CHAR8  **Message
  );

// Optional host message after a successful flash operation.
CONST CHAR8 *
FastbootFlashInfo (
  CONST CHAR8  *Name
  );

EFI_STATUS
FastbootGetVar (
  CONST CHAR8  *Name,
  CHAR8        *Value,
  UINTN        Size
  );

EFI_STATUS
FastbootFlash (
  CONST CHAR8                *Name,
  CONST VOID                 *Data,
  UINTN                      Size,
  FASTBOOT_STORAGE_PROGRESS  Progress
  );

EFI_STATUS
FastbootRead (
  CONST CHAR8                *Name,
  UINT64                     Offset,
  UINTN                      Size,
  VOID                       *Data,
  FASTBOOT_STORAGE_PROGRESS  Progress OPTIONAL
  );
