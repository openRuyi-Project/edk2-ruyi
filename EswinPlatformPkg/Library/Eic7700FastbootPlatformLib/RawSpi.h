/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once

#include <Library/Eic7700SpiNorLib.h>
#include <Library/FastbootStorageLib.h>

EFI_STATUS
Eic7700FastbootRawFlash (
  EIC7700_SPI_NOR *Nor OPTIONAL, UINT32 Capacity,
  CONST VOID *Data, UINTN Size, FASTBOOT_STORAGE_PROGRESS Progress OPTIONAL
  );
