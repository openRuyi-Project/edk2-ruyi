/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/PlatformFirmwareUpdateLib.h>
#include <Library/PlatformSettingsLib.h>
#include <IndustryStandard/UnmatchedFirmwareDefaults.h>
#include <Library/Fu740SpiNorLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "Internal.h"

// A failed erase can damage FMAP. Permit a retry only in this same updater
// session after its original layout was checked and writing began.
STATIC BOOLEAN  mRecoveryAllowed;

VOID
PlatformFirmwareUpdateGetInfo (
  PLATFORM_FIRMWARE_UPDATE_INFO  *Info
  )
{
  Info->ImageSize          = UNMATCHED_FIRMWARE_SIZE;
  Info->PreservationNotice = L"UEFI variables and platform settings are preserved.";
}

STATIC EFI_STATUS
ReadFlash (
  UINT32  Offset,
  UINTN   Size,
  VOID    *Buffer
  )
{
  EFI_TPL     Tpl    = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  Status = Fu740SpiNorRead (Offset, Size, Buffer);

  gBS->RestoreTPL (Tpl);
  return Status;
}

EFI_STATUS
PlatformFirmwareUpdateValidate (
  CONST VOID  *Image,
  UINTN       Size
  )
{
  UINT8       Map[UNMATCHED_FMAP_SIZE];
  EFI_STATUS  Status = UnmatchedFirmwareImageValid (Image, Size);

  if (!EFI_ERROR (Status)) {
    EFI_TPL  Tpl = gBS->RaiseTPL (TPL_NOTIFY);
    Status = Fu740SpiNorInit (FALSE);
    gBS->RestoreTPL (Tpl);
  }

  if (!EFI_ERROR (Status)) {
    Status = ReadFlash (UNMATCHED_FMAP_OFFSET, sizeof (Map), Map);
  }

  if (!EFI_ERROR (Status) && !UnmatchedFirmwareMapValid (Map) && !mRecoveryAllowed) {
    Status = EFI_UNSUPPORTED;
  }

  return Status;
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
  BOOLEAN PreserveDefaults = FALSE;
  RUYI_SETTINGS_RECORD SavedDefaults;
  UINTN                               Done = 0, Total = UNMATCHED_FIRMWARE_SIZE;
  EFI_STATUS                          Status = PlatformFirmwareUpdateValidate (Image, Size);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Saved   = AllocatePool (UNMATCHED_NV_SIZE);
  Scratch = AllocatePool (SIZE_64KB);
  if (!Saved || !Scratch) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Out;
  }

  Status = FirmwareUpdateFreezeNv (&Fvb, &Attributes);
  if (EFI_ERROR (Status)) {
    goto Out;
  }

  Status = Notify (Progress, FirmwareUpdateRead, 0, UNMATCHED_NV_SIZE, Context);
  if (EFI_ERROR (Status)) {
    goto Out;
  }

  for (UINT32 Pos = 0; Pos < UNMATCHED_NV_SIZE; Pos += SIZE_64KB) {
    UINT32 Offset = UNMATCHED_NV_OFFSET + Pos;
    Status = ReadFlash (Offset, SIZE_64KB, Saved + Pos);
    if (EFI_ERROR (Status)) {
      goto Out;
    }
  }

  EFI_TPL DefaultsTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = PlatformSettingsSnapshot (&SavedDefaults);
  gBS->RestoreTPL (DefaultsTpl);
  if (!EFI_ERROR (Status)) {
    PreserveDefaults = TRUE;
    Total -= UNMATCHED_CONFIG_SIZE;
  } else if (Status != EFI_NOT_FOUND) { goto Out; }

  // Update the first-stage boot block last. Every changed sector is verified
  // before moving on; unchanged sectors and the variable store is kept.
  for (UINT32 Index = 1; Index <= UNMATCHED_FIRMWARE_SIZE / SIZE_64KB; Index++) {
    UINT32  Offset = Index == UNMATCHED_FIRMWARE_SIZE / SIZE_64KB ? 0 : Index * SIZE_64KB;
    if (PreserveDefaults && Offset == UNMATCHED_CONFIG_OFFSET) { continue; }
    CONST UINT8 *Expected = Image + Offset;
    Status = Notify (Progress, FirmwareUpdateRead, Done, Total, Context);
    if (!EFI_ERROR (Status)) {
      Status = ReadFlash (Offset, SIZE_64KB, Scratch);
    }

    if (EFI_ERROR (Status)) {
      goto Out;
    }

    if (CompareMem (Scratch, Expected, SIZE_64KB)) {
      Status = Notify (Progress, FirmwareUpdateErase, Done, Total, Context);
      if (EFI_ERROR (Status)) {
        goto Out;
      }

      EFI_TPL  Tpl = gBS->RaiseTPL (TPL_NOTIFY);
      mRecoveryAllowed = TRUE;
      Status           = Fu740SpiNorEraseBlock (Offset, SIZE_64KB);
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
          if (Expected[Page + I] != 0xff) {
            Empty = FALSE;
            break;
          }
        }

        if (Empty) {
          continue;
        }

        Tpl    = gBS->RaiseTPL (TPL_NOTIFY);
        Status = Fu740SpiNorWrite (Offset + Page, 256, Expected + Page);
        gBS->RestoreTPL (Tpl);
        if (EFI_ERROR (Status)) {
          goto Out;
        }
      }

      Status = Notify (Progress, FirmwareUpdateVerify, Done, Total, Context);
      if (!EFI_ERROR (Status)) {
        Status = ReadFlash (Offset, SIZE_64KB, Scratch);
      }

      if (!EFI_ERROR (Status) && CompareMem (Scratch, Expected, SIZE_64KB)) {
        Status = EFI_CRC_ERROR;
      }

      if (EFI_ERROR (Status)) {
        goto Out;
      }
    }

    Done += SIZE_64KB;
  }

  for (UINT32 Pos = 0; Pos < UNMATCHED_NV_SIZE; Pos += SIZE_64KB) {
    UINT32 Offset = UNMATCHED_NV_OFFSET + Pos;
    Status = ReadFlash (Offset, SIZE_64KB, Scratch);
    if (!EFI_ERROR (Status) && CompareMem (Scratch, Saved + Pos, SIZE_64KB)) {
      Status = EFI_CRC_ERROR;
    }

    if (EFI_ERROR (Status)) {
      goto Out;
    }
  }

  Status = Notify (Progress, FirmwareUpdateVerify, Total, Total, Context);
  if (!EFI_ERROR (Status)) {
    mRecoveryAllowed = FALSE;
  }

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
