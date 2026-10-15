/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>
#include <Protocol/SpiNorFlash.h>
#include <Pi/PiFirmwareVolume.h>
#include <Protocol/FirmwareVolumeBlock.h>

#define K3_FIRMWARE_SIZE       SIZE_8MB
#define K3_FIRMWARE_NV_OFFSET  0x700000U
#define K3_FIRMWARE_NV_SIZE    0xc0000U

typedef enum {
  K3FirmwareRead,
  K3FirmwareErase,
  K3FirmwareWrite,
  K3FirmwareVerify
} K3_FIRMWARE_PHASE;

typedef EFI_STATUS (*K3_FIRMWARE_PROGRESS)(
  K3_FIRMWARE_PHASE  Phase,
  UINT64             Done,
  UINT64             Total,
  VOID               *Context
  );

EFI_STATUS
K3FirmwareValidate (
  CONST VOID  *Image,
  UINTN       Size
  );

// Program a complete normal SPI image, retaining the entire NV reservation.
// Raw images are checked for integrity and board/layout compatibility, not signed.
EFI_STATUS
K3FirmwareUpdate (
  EFI_SPI_NOR_FLASH_PROTOCOL  *Nor,
  CONST VOID                  *Image,
  UINTN                       Size,
  K3_FIRMWARE_PROGRESS        Progress OPTIONAL,
  VOID                        *Context OPTIONAL
  );

// Freeze the active variable store while its physical reservation is saved.
// Recovery has no FVB and returns success with *Fvb == NULL.
EFI_STATUS
K3FirmwareFreezeNv (
  EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  **Fvb,
  EFI_FVB_ATTRIBUTES_2                *Attributes
  );
