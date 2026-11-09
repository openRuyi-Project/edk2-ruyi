/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Library/NvFvbLib.h>

#define P550_FIRMWARE_BYTES  0x600000U
#define P550_NV_OFFSET      P550_FIRMWARE_BYTES

// Bind the fixed Ruyi Firmware variable region to the SPI backend.
EFI_STATUS PremierP550NvFlashInitialize (BOOLEAN Runtime, NV_FLASH_OPS *Ops);
