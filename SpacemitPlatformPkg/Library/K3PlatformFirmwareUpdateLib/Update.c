/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/PlatformFirmwareUpdateLib.h>
#include <Library/K3FirmwareUpdateLib.h>
#include <Library/K3SpiNorLib.h>

typedef struct {
  FIRMWARE_UPDATE_PROGRESS    Progress;
  VOID                        *Context;
} K3_UPDATE_CONTEXT;

VOID
PlatformFirmwareUpdateGetInfo (
  PLATFORM_FIRMWARE_UPDATE_INFO  *Info
  )
{
  Info->ImageSize          = K3_FIRMWARE_SIZE;
  Info->PreservationNotice = L"UEFI variables and CPU selection are preserved. DDR training is reset.";
}

EFI_STATUS
PlatformFirmwareUpdateValidate (
  CONST VOID  *Image,
  UINTN       Size
  )
{
  EFI_SPI_NOR_FLASH_PROTOCOL  *Nor;
  EFI_STATUS                  Status = K3FirmwareValidate (Image, Size);

  if (!EFI_ERROR (Status)) {
    Status = K3SpiNorFindFlash (&Nor);
  }

  return Status;
}

STATIC EFI_STATUS
Progress (
  K3_FIRMWARE_PHASE  Phase,
  UINT64             Done,
  UINT64             Total,
  VOID               *Context
  )
{
  K3_UPDATE_CONTEXT  *Update = Context;

  return Update->Progress ((FIRMWARE_UPDATE_PHASE)Phase, Done, Total, Update->Context);
}

EFI_STATUS
PlatformFirmwareUpdate (
  CONST VOID                *Image,
  UINTN                     Size,
  FIRMWARE_UPDATE_PROGRESS  Notify,
  VOID                      *Context
  )
{
  EFI_SPI_NOR_FLASH_PROTOCOL  *Nor;
  K3_UPDATE_CONTEXT           Update = { Notify, Context };
  EFI_STATUS                  Status = K3SpiNorFindFlash (&Nor);

  return EFI_ERROR (Status) ? Status : K3FirmwareUpdate (Nor, Image, Size, Notify ? Progress : NULL, &Update);
}
