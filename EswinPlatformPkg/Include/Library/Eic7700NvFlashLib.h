/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Library/NvFvbLib.h>

#define EIC7700_FIRMWARE_BYTES  0x600000U
#define EIC7700_NV_OFFSET      EIC7700_FIRMWARE_BYTES

// Bind the fixed Ruyi Firmware variable region to the SPI backend.
EFI_STATUS Eic7700NvFlashInitialize (BOOLEAN Runtime, NV_FLASH_OPS *Ops);
