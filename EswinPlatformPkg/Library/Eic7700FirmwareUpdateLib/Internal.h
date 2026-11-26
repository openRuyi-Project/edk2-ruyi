/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>
#include <Library/Eic7700BoardProfileLib.h>
#include <Library/FirmwareUpdateLib.h>
#include <Library/D560BootFlashLib.h>
#define EIC7700_SPI_IMAGE_BYTES SIZE_8MB
EFI_STATUS Eic7700FirmwareRomValid (CONST UINT8 *Rom, UINTN Size, CONST COREBOOT_IMAGE_LAYOUT *Layout);
EFI_STATUS Eic7700FirmwareImageValid (CONST VOID *Image, UINTN Size);

#define D560_DIE_COUNT          2
#define D560_UPDATE_BYTES       SIZE_16MB
#define D560_STAGE_START        0x50000U
#define D560_DIE0_STAGE_LIMIT   0x600000U
#define D560_DIE1_STAGE_LIMIT   0x680000U

typedef struct {
  CONST UINT8 *Image;
  UINT32      Size;
  UINTN       TableBytes;
} D560_FIRMWARE_DIE;

EFI_STATUS
D560FirmwarePair (
  CONST VOID *Buffer, UINTN Size, CONST EIC7700_BOARD_PROFILE *Board,
  D560_FIRMWARE_DIE Dies[D560_DIE_COUNT]
  );

EFI_STATUS D560ProgramPair (CONST D560_FIRMWARE_DIE Dies[D560_DIE_COUNT], FIRMWARE_UPDATE_PROGRESS Progress, VOID *Context);
