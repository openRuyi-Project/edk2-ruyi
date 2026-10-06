/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_FASTBOOT_SPI_H
#define K3_FASTBOOT_SPI_H
#include <Library/FastbootStorageLib.h>
#define K3_SPI_TARGET       "spi"
#define K3_FIRMWARE_TARGET  "firmware"
UINT64
K3FastbootSpiSize (
  VOID
  );

EFI_STATUS
K3FastbootSpiInitialize (
  VOID
  );

VOID
K3FastbootSpiStop (
  VOID
  );

EFI_STATUS
K3FastbootSpiRead (
  UINT64                     Offset,
  UINTN                      Bytes,
  VOID                       *Data,
  FASTBOOT_STORAGE_PROGRESS  Progress OPTIONAL
  );

EFI_STATUS
K3FastbootSpiFlash (
  CONST VOID                 *Data,
  UINTN                      Bytes,
  FASTBOOT_STORAGE_PROGRESS  Progress
  );

BOOLEAN
K3FastbootSpiAvailable (
  VOID
  );

EFI_STATUS
K3FastbootFirmwareFlash (
  CONST VOID                 *Data,
  UINTN                      Bytes,
  FASTBOOT_STORAGE_PROGRESS  Progress
  );

#endif
