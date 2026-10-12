/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>
#include <Library/FastbootStorageLib.h>

#define K3_EC_VERSION_SIZE  32
#define K3_EC_UPDATE_SIZE   SIZE_256KB

EFI_STATUS
K3EcGetVersion (
  CHAR8  Version[K3_EC_VERSION_SIZE]
  );

EFI_STATUS
K3EcFlashRead (
  UINT32                     Offset,
  UINTN                      Size,
  VOID                       *Data,
  FASTBOOT_STORAGE_PROGRESS  Progress OPTIONAL
  );

EFI_STATUS
K3EcUpdate (
  CONST VOID                 *Image,
  UINTN                      Size,
  FASTBOOT_STORAGE_PROGRESS  Progress OPTIONAL
  );

EFI_STATUS
K3EcReboot (
  VOID
  );
