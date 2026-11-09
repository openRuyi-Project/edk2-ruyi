/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>
#define P550_SPI_IMAGE_BYTES SIZE_8MB
EFI_STATUS PremierP550FirmwareImageValid (CONST VOID *Image, UINTN Size);
