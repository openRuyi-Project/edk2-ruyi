/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IndustryStandard/Eic7700BootConfig.h>
#include <Library/PlatformFirmwareUpdateLib.h>
#include <Library/PlatformSettingsLib.h>
#include <IndustryStandard/P550FirmwareDefaults.h>
#include <Library/Eic7700SpiNorLib.h>
#include <Library/PremierP550NvFlashLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "Internal.h"

VOID
PlatformFirmwareUpdateGetInfo (
  PLATFORM_FIRMWARE_UPDATE_INFO  *Info
  )
{
  Info->ImageSize          = P550_SPI_IMAGE_BYTES;
  Info->PreservationNotice = L"UEFI variables and DDR ECC settings are preserved.";
}

STATIC EFI_STATUS
ReadFlash (
  UINT32  Offset,
  UINTN   Size,
  VOID    *Buffer
  )
{
  EFI_TPL     Tpl    = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  Status = Eic7700SpiNorRead (Offset, Size, Buffer);

  gBS->RestoreTPL (Tpl);
  return Status;
}

EFI_STATUS
PlatformFirmwareUpdateValidate (
  CONST VOID  *Image,
  UINTN       Size
  )
{
  EFI_STATUS Status = PremierP550FirmwareImageValid (Image, Size);
  if (!EFI_ERROR (Status)) { Status = Eic7700SpiNorInit (FALSE); }
  if (!EFI_ERROR (Status) && Eic7700SpiNorSize () < P550_SPI_IMAGE_BYTES) { Status = EFI_UNSUPPORTED; }
  return Status;
}

STATIC BOOLEAN mPreserveDefaults;

STATIC BOOLEAN
Preserved (
  UINT32  Offset
  )
{
  return (mPreserveDefaults && Offset >= P550_DEFAULTS_OFFSET &&
          Offset < P550_DEFAULTS_OFFSET + SETTINGS_SLOT_COUNT * P550_DEFAULTS_STRIDE) ||
         (Offset >= P550_NV_OFFSET && Offset < P550_NV_OFFSET + NV_TOTAL_SIZE) ||
         (Offset >= EIC7700_BOOT_CONFIG_OFFSET &&
          Offset < EIC7700_BOOT_CONFIG_OFFSET +
                   EIC7700_BOOT_CONFIG_SLOT_COUNT * EIC7700_BOOT_CONFIG_SLOT_SIZE);
}

STATIC EFI_STATUS
Notify (
  FIRMWARE_UPDATE_PROGRESS  Progress,
  FIRMWARE_UPDATE_PHASE     Phase,
  UINT64                    Done,
  UINT64                    Total,
  VOID                      *Context
  )
{
  return Progress ? Progress (Phase, Done, Total, Context) : EFI_SUCCESS;
}

EFI_STATUS
PlatformFirmwareUpdate (
  CONST VOID                *Buffer,
  UINTN                     Size,
  FIRMWARE_UPDATE_PROGRESS  Progress,
  VOID                      *Context
  )
{
  CONST UINT8                         *Image = Buffer;
  EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *Fvb   = NULL;
  EFI_FVB_ATTRIBUTES_2                Attributes;
  UINT8                               *Saved = NULL, *Scratch = NULL;
  UINTN                               Done = 0, Total = P550_SPI_IMAGE_BYTES - NV_TOTAL_SIZE -
      EIC7700_BOOT_CONFIG_SLOT_COUNT * EIC7700_BOOT_CONFIG_SLOT_SIZE;
  EFI_STATUS                          Status = PlatformFirmwareUpdateValidate (Image, Size);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Saved   = AllocatePool (NV_TOTAL_SIZE);
  Scratch = AllocatePool (SIZE_64KB);
  if (!Saved || !Scratch) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Out;
  }

  RUYI_SETTINGS_RECORD Defaults;
  EFI_TPL DefaultsTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = PlatformSettingsSnapshot (&Defaults);
  gBS->RestoreTPL (DefaultsTpl);
  mPreserveDefaults = !EFI_ERROR (Status);
  if (EFI_ERROR (Status) && Status != EFI_NOT_FOUND) { goto Out; }
  if (mPreserveDefaults) { Total -= SETTINGS_SLOT_COUNT * P550_DEFAULTS_STRIDE; }
  Status = FirmwareUpdateFreezeNv (&Fvb, &Attributes);
  if (EFI_ERROR (Status)) {
    goto Out;
  }

  Status = Notify (Progress, FirmwareUpdateRead, 0, NV_TOTAL_SIZE, Context);
  if (EFI_ERROR (Status)) {
    goto Out;
  }

  for (UINT32 Pos = 0; Pos < NV_TOTAL_SIZE; Pos += SIZE_64KB) {
    UINT32  Offset = P550_NV_OFFSET + Pos;
    Status = ReadFlash (Offset, SIZE_64KB, Saved + Pos);
    if (EFI_ERROR (Status)) {
      goto Out;
    }
  }

  // Update the first-stage boot block last. Every changed sector is verified
  // before moving on; unchanged sectors and the variable region are kept.
  for (UINT32 Index = 1; Index <= P550_SPI_IMAGE_BYTES / SIZE_64KB; Index++) {
    UINT32  Offset = Index == P550_SPI_IMAGE_BYTES / SIZE_64KB ? 0 : Index * SIZE_64KB;
    if (Preserved (Offset)) {
      continue;
    }

    Status = Notify (Progress, FirmwareUpdateRead, Done, Total, Context);
    if (!EFI_ERROR (Status)) {
      Status = ReadFlash (Offset, SIZE_64KB, Scratch);
    }

    if (EFI_ERROR (Status)) {
      goto Out;
    }

    if (CompareMem (Scratch, Image + Offset, SIZE_64KB)) {
      Status = Notify (Progress, FirmwareUpdateErase, Done, Total, Context);
      if (EFI_ERROR (Status)) {
        goto Out;
      }

      EFI_TPL  Tpl = gBS->RaiseTPL (TPL_NOTIFY);
      Status           = Eic7700SpiNorErase (Offset, SIZE_64KB);
      gBS->RestoreTPL (Tpl);
      if (EFI_ERROR (Status)) {
        goto Out;
      }

      Status = Notify (Progress, FirmwareUpdateWrite, Done, Total, Context);
      if (EFI_ERROR (Status)) {
        goto Out;
      }

      for (UINT32 Page = 0; Page < SIZE_64KB; Page += 256) {
        BOOLEAN  Empty = TRUE;
        for (UINTN I = 0; I < 256; I++) {
          if (Image[Offset + Page + I] != 0xff) {
            Empty = FALSE;
            break;
          }
        }

        if (Empty) {
          continue;
        }

        Tpl    = gBS->RaiseTPL (TPL_NOTIFY);
        Status = Eic7700SpiNorWrite (Offset + Page, 256, Image + Offset + Page);
        gBS->RestoreTPL (Tpl);
        if (EFI_ERROR (Status)) {
          goto Out;
        }
      }

      Status = Notify (Progress, FirmwareUpdateVerify, Done, Total, Context);
      if (!EFI_ERROR (Status)) {
        Status = ReadFlash (Offset, SIZE_64KB, Scratch);
      }

      if (!EFI_ERROR (Status) && CompareMem (Scratch, Image + Offset, SIZE_64KB)) {
        Status = EFI_CRC_ERROR;
      }

      if (EFI_ERROR (Status)) {
        goto Out;
      }
    }

    Done += SIZE_64KB;
  }

  for (UINT32 Pos = 0; Pos < NV_TOTAL_SIZE; Pos += SIZE_64KB) {
    UINT32  Offset = P550_NV_OFFSET + Pos;
    Status = ReadFlash (Offset, SIZE_64KB, Scratch);
    if (!EFI_ERROR (Status) && CompareMem (Scratch, Saved + Pos, SIZE_64KB)) {
      Status = EFI_CRC_ERROR;
    }

    if (EFI_ERROR (Status)) {
      goto Out;
    }
  }

  Status = Notify (Progress, FirmwareUpdateVerify, Total, Total, Context);

Out:
  if (Fvb) {
    EFI_STATUS  Restore = Fvb->SetAttributes (Fvb, &Attributes);
    if (!EFI_ERROR (Status)) {
      Status = Restore;
    }
  }

  if (Scratch) {
    FreePool (Scratch);
  }

  if (Saved) {
    FreePool (Saved);
  }

  return Status;
}
