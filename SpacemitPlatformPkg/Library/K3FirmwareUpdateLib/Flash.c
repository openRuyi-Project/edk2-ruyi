/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/PlatformSettingsLib.h>
#include <IndustryStandard/K3FirmwareDefaults.h>
#include <Library/FirmwareUpdateLib.h>
#include <Guid/K3DdrTraining.h>
#include <Library/K3CpuConfigLib.h>
#include <Protocol/SpiNorFlash.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/K3FirmwareUpdateLib.h>
#include "Internal.h"

EFI_STATUS
K3FirmwareFreezeNv (
  EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL **Fvb,
  EFI_FVB_ATTRIBUTES_2 *Attributes
  )
{
  return FirmwareUpdateFreezeNv (Fvb, Attributes);
}

STATIC BOOLEAN
EraseOnly (
  UINT32  Offset
  )
{
  // DDR training results must be regenerated for the new firmware.
  return ((Offset >= K3_DDR_TRAINING_OFFSET) &&
          (Offset < K3_DDR_TRAINING_OFFSET + SIZE_64KB));
}

STATIC BOOLEAN
InNv (
  UINT32  Offset
  )
{
  return Offset >= K3_FIRMWARE_NV_OFFSET &&
         Offset < K3_FIRMWARE_NV_OFFSET + K3_FIRMWARE_NV_SIZE;
}

EFI_STATUS
K3FirmwareUpdate (
  EFI_SPI_NOR_FLASH_PROTOCOL  *Nor,
  CONST VOID                  *Buffer,
  UINTN                       Size,
  K3_FIRMWARE_PROGRESS        Progress,
  VOID                        *Context
  )
{
  CONST UINT8                         *Image = Buffer;
  EFI_STATUS                          Status;
  UINT8                               *Nv, *Scratch, *Cpu = NULL, *Defaults = NULL;
  RUYI_SETTINGS_RECORD SavedDefaults;
  BOOLEAN PreserveDefaults = FALSE, PreserveCpu = FALSE;
  EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *Fvb = NULL;
  EFI_FVB_ATTRIBUTES_2                Attributes;
  UINT64                              Done, Total = K3_FIRMWARE_SIZE - K3_FIRMWARE_NV_SIZE;

  if (!Nor) {
    return EFI_NOT_READY;
  }

  if ((Nor->FlashSize != K3_FIRMWARE_SIZE) || Nor->EraseBlockBytes != SIZE_4KB ||
      Nor->EraseBlockBytes % SIZE_4KB ||
      K3_FIRMWARE_NV_OFFSET % Nor->EraseBlockBytes ||
      K3_FIRMWARE_NV_SIZE % Nor->EraseBlockBytes ||
      K3_FIRMWARE_SIZE % Nor->EraseBlockBytes)
  {
    return EFI_UNSUPPORTED;
  }

  Status = K3FirmwareValidate (Buffer, Size);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Nv      = AllocatePool (K3_FIRMWARE_NV_SIZE);
  Scratch = AllocatePool (SIZE_64KB);
  Cpu = AllocatePool (SIZE_4KB);
  Defaults = AllocateCopyPool (SETTINGS_SLOT_COUNT * K3_DEFAULTS_STRIDE, Image + K3_DEFAULTS_OFFSET);
  if (!Nv || !Scratch || !Cpu || !Defaults) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Out;
  }

  // Require the installed layout to use the same fixed regions and NV bounds.
  Status = Nor->ReadData (Nor, FMAP_OFFSET, FMAP_SIZE, Scratch);
  if (EFI_ERROR (Status)) {
    goto Out;
  }

  if (!K3FirmwareMapValid (Scratch)) {
    Status = EFI_UNSUPPORTED;
    goto Out;
  }

  Status = K3FirmwareFreezeNv (&Fvb, &Attributes);
  if (EFI_ERROR (Status)) {
    goto Out;
  }

  EFI_TPL DefaultsTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = PlatformSettingsSnapshot (&SavedDefaults);
  gBS->RestoreTPL (DefaultsTpl);
  if (!EFI_ERROR (Status)) {
    PreserveDefaults = TRUE;
    Total -= SETTINGS_SLOT_COUNT * K3_DEFAULTS_STRIDE;
    Status = Nor->ReadData (Nor, K3_DEFAULTS_OFFSET, SETTINGS_SLOT_COUNT * K3_DEFAULTS_STRIDE, Defaults);
    if (EFI_ERROR (Status)) { goto Out; }
  } else if (Status != EFI_NOT_FOUND) { goto Out; }
  Status = Nor->ReadData (Nor, K3_CPU_CONFIG_OFFSET, SIZE_4KB, Cpu);
  if (EFI_ERROR (Status)) { goto Out; }
  // Only the supported CPU record is retained. An invalid record selects X100.
  if (!K3CpuConfigValid ((CONST K3_CPU_CONFIG_RECORD *)Cpu)) {
    SetMem (Cpu, SIZE_4KB, 0xff);
  } else {
    PreserveCpu = TRUE;
    Total -= SIZE_4KB;
  }

  for (UINT32 Offset = 0; Offset < K3_FIRMWARE_NV_SIZE; Offset += SIZE_64KB) {
    if (Progress) {
      Status = Progress (K3FirmwareRead, Offset, K3_FIRMWARE_NV_SIZE, Context);
      if (EFI_ERROR (Status)) {
        goto Out;
      }
    }

    Status = Nor->ReadData (Nor, K3_FIRMWARE_NV_OFFSET + Offset, SIZE_64KB, Nv + Offset);
    if (EFI_ERROR (Status)) {
      goto Out;
    }
  }

  if (Progress) {
    Status = Progress (K3FirmwareRead, K3_FIRMWARE_NV_SIZE, K3_FIRMWARE_NV_SIZE, Context);
    if (EFI_ERROR (Status)) {
      goto Out;
    }
  }

  // Submit a larger range so the SFDP driver can select block erase commands.
  // The fixed image and NV boundaries are all aligned to 64 KiB.
  UINT32  EraseBytes = Nor->EraseBlockBytes;

  if ((EraseBytes < SIZE_64KB) && (SIZE_64KB % EraseBytes == 0)) {
    EraseBytes = SIZE_64KB;
  }

  for (K3_FIRMWARE_PHASE Phase = K3FirmwareErase; Phase <= K3FirmwareVerify; Phase++) {
    UINT32 Step;
    Done = 0;
    for (UINT32 Offset = 0; Offset < K3_FIRMWARE_SIZE; Offset += Step) {
      Step = Phase == K3FirmwareErase ? EraseBytes : SIZE_4KB;
      // Split erase blocks around live settings. Do not erase and rewrite both slots.
      if (Phase == K3FirmwareErase &&
          ((Offset % EraseBytes) ||
           (PreserveDefaults && Offset < K3_DEFAULTS_OFFSET + SETTINGS_SLOT_COUNT * K3_DEFAULTS_STRIDE &&
            Offset + Step > K3_DEFAULTS_OFFSET) ||
           (PreserveCpu && Offset <= K3_CPU_CONFIG_OFFSET && Offset + Step > K3_CPU_CONFIG_OFFSET))) {
        Step = SIZE_4KB;
      }
      BOOLEAN Keep = InNv (Offset) ||
        (PreserveCpu && Offset == K3_CPU_CONFIG_OFFSET) ||
        (PreserveDefaults && Offset >= K3_DEFAULTS_OFFSET &&
         Offset < K3_DEFAULTS_OFFSET + SETTINGS_SLOT_COUNT * K3_DEFAULTS_STRIDE);
      if (Keep && Phase != K3FirmwareVerify) { continue; }

      if (Progress) {
        Status = Progress (Phase, Done, Phase == K3FirmwareVerify ? Size : Total, Context);
        if (EFI_ERROR (Status)) {
          goto Out;
        }
      }

      if (Phase == K3FirmwareErase) {
        Status = Nor->Erase (Nor, Offset, Step / SIZE_4KB);
      } else if (Phase == K3FirmwareWrite) {
        // Discard training data even when the input is a saved SPI image.
        if (EraseOnly (Offset)) {
          Status = EFI_SUCCESS;
        } else {
          Status = Nor->WriteData (Nor, Offset, Step, Offset == K3_CPU_CONFIG_OFFSET ? Cpu :
                                    (Offset >= K3_DEFAULTS_OFFSET && Offset < K3_DEFAULTS_OFFSET + SETTINGS_SLOT_COUNT * K3_DEFAULTS_STRIDE) ?
                                    Defaults + Offset - K3_DEFAULTS_OFFSET : (UINT8 *)Image + Offset);
        }
      } else {
        Status = Nor->ReadData (Nor, Offset, Step, Scratch);
        CONST UINT8  *Expected = InNv (Offset) ? Nv + Offset - K3_FIRMWARE_NV_OFFSET :
                                 Offset == K3_CPU_CONFIG_OFFSET ? Cpu :
                                 (Offset >= K3_DEFAULTS_OFFSET && Offset < K3_DEFAULTS_OFFSET + SETTINGS_SLOT_COUNT * K3_DEFAULTS_STRIDE) ?
                                 Defaults + Offset - K3_DEFAULTS_OFFSET : Image + Offset;
        if (!EFI_ERROR (Status)) {
          if (EraseOnly (Offset)) {
            for (UINT32 Index = 0; Index < Step; Index++) {
              if (Scratch[Index] != 0xff) {
                Status = EFI_CRC_ERROR;
                break;
              }
            }
          } else if (CompareMem (Scratch, Expected, Step)) {
            Status = EFI_CRC_ERROR;
          }
        }
      }

      if (EFI_ERROR (Status)) {
        goto Out;
      }

      Done += Step;
    }

    if (Progress) {
      Status = Progress (Phase, Done, Done, Context);
      if (EFI_ERROR (Status)) {
        goto Out;
      }
    }
  }

Out:
  if (Fvb) {
    EFI_STATUS  Restore = Fvb->SetAttributes (Fvb, &Attributes);
    if (!EFI_ERROR (Status)) {
      Status = Restore;
    }
  }

  if (Nv) {
    FreePool (Nv);
  }

  if (Scratch) {
    FreePool (Scratch);
  }
  if (Cpu) { FreePool (Cpu); }
  if (Defaults) { FreePool (Defaults); }

  return Status;
}
