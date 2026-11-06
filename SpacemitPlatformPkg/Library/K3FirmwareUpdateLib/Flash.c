/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
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
  // Firmware updates reset CPU selection and discard saved DDR training.
  return ((Offset >= K3_DDR_TRAINING_OFFSET) &&
          (Offset < K3_DDR_TRAINING_OFFSET + SIZE_64KB)) ||
         (Offset == K3_CPU_CONFIG_OFFSET);
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
  UINT8                               *Nv, *Scratch;
  EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *Fvb = NULL;
  EFI_FVB_ATTRIBUTES_2                Attributes;
  UINT64                              Done, Total = K3_FIRMWARE_SIZE - K3_FIRMWARE_NV_SIZE;

  if (!Nor) {
    return EFI_NOT_READY;
  }

  if ((Nor->FlashSize != K3_FIRMWARE_SIZE) || !Nor->EraseBlockBytes ||
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
  if (!Nv || !Scratch) {
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
    UINT32  Step = Phase == K3FirmwareErase ? EraseBytes : SIZE_4KB;
    Done = 0;
    for (UINT32 Offset = 0; Offset < K3_FIRMWARE_SIZE; Offset += Step) {
      if (InNv (Offset) && (Phase != K3FirmwareVerify)) {
        continue;
      }

      if (Progress) {
        Status = Progress (Phase, Done, Phase == K3FirmwareVerify ? Size : Total, Context);
        if (EFI_ERROR (Status)) {
          goto Out;
        }
      }

      if (Phase == K3FirmwareErase) {
        Status = Nor->Erase (Nor, Offset, Step / SIZE_4KB);
      } else if (Phase == K3FirmwareWrite) {
        // Discard CPU selection and training data even for saved SPI images.
        if (EraseOnly (Offset)) {
          Status = EFI_SUCCESS;
        } else {
          Status = Nor->WriteData (Nor, Offset, Step, (UINT8 *)Image + Offset);
        }
      } else {
        Status = Nor->ReadData (Nor, Offset, Step, Scratch);
        CONST UINT8  *Expected = InNv (Offset) ? Nv + Offset - K3_FIRMWARE_NV_OFFSET
                                             : Image + Offset;
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

  return Status;
}
