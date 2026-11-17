/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>
#define EIC7700_SPI_IMAGE_BYTES SIZE_8MB
EFI_STATUS Eic7700FirmwareImageValid (CONST VOID *Image, UINTN Size);
