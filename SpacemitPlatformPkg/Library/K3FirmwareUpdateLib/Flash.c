/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/NvVarStoreFormatted.h>
#include <Guid/K3DdrTraining.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SpiNorFlash.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/K3FirmwareUpdateLib.h>
#include "Internal.h"

STATIC BOOLEAN
ControllerMatches (
  CONST EFI_SPI_NOR_FLASH_PROTOCOL  *Nor
  )
{
  if (!Nor || !Nor->SpiPeripheral || !Nor->SpiPeripheral->SpiBus ||
      !Nor->SpiPeripheral->SpiBus->ControllerPath)
  {
    return FALSE;
  }

  CONST MEMMAP_DEVICE_PATH  *Path = (CONST VOID *)Nor->SpiPeripheral->SpiBus->ControllerPath;

  return Path->Header.Type == HARDWARE_DEVICE_PATH && Path->Header.SubType == HW_MEMMAP_DP &&
         (Path->Header.Length[0] | (Path->Header.Length[1] << 8)) == sizeof (*Path) &&
         Path->StartingAddress == 0xd420c000 && Path->EndingAddress == 0xd420cfff;
}

EFI_STATUS
K3FirmwareFindFlash (
  EFI_SPI_NOR_FLASH_PROTOCOL  **Result
  )
{
  EFI_HANDLE  *Handles;
  UINTN       Count;
  EFI_STATUS  Status;

  *Result = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiSpiNorFlashProtocolGuid,
                   NULL,
                   &Count,
                   &Handles
                   );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  EFI_SPI_NOR_FLASH_PROTOCOL  *Found = NULL;

  for (UINTN Index = 0; Index < Count; Index++) {
    EFI_SPI_NOR_FLASH_PROTOCOL  *Nor;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSpiNorFlashProtocolGuid, (VOID **)&Nor);
    if (EFI_ERROR (Status) || !ControllerMatches (Nor)) {
      continue;
    }

    if (Found || !Nor->FlashSize || (Nor->EraseBlockBytes < SIZE_4KB) ||
        Nor->EraseBlockBytes % SIZE_4KB || Nor->FlashSize % Nor->EraseBlockBytes ||
        !Nor->GetFlashid || !Nor->ReadData || !Nor->WriteData || !Nor->Erase)
    {
      Status = EFI_UNSUPPORTED;
      goto Done;
    }

    UINT8  Id[3];
    Status = Nor->GetFlashid (Nor, Id);
    if (EFI_ERROR (Status)) {
      goto Done;
    }

    if (CompareMem (Id, Nor->Deviceid, 3)) {
      Status = EFI_UNSUPPORTED;
      goto Done;
    }

    Found = Nor;
  }

  *Result = Found;
  Status  = Found ? EFI_SUCCESS : EFI_NOT_FOUND;
Done:
  FreePool (Handles);
  return Status;
}

EFI_STATUS
K3FirmwareFreezeNv (
  EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  **Fvb,
  EFI_FVB_ATTRIBUTES_2                *Attributes
  )
{
  EFI_HANDLE  *Handles;
  UINTN       Count;
  EFI_STATUS  Status;

  *Fvb   = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEdkiiNvVarStoreFormattedGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (Status == EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Count != 1) {
    FreePool (Handles);
    return EFI_UNSUPPORTED;
  }

  Status = gBS->HandleProtocol (Handles[0], &gEfiFirmwareVolumeBlockProtocolGuid, (VOID **)Fvb);
  FreePool (Handles);
  // RAM recovery publishes variable readiness without a flash-backed FVB.
  if (Status == EFI_UNSUPPORTED) {
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = (*Fvb)->GetAttributes (*Fvb, Attributes);
  if (!EFI_ERROR (Status)) {
    EFI_FVB_ATTRIBUTES_2  Protected = *Attributes & ~EFI_FVB2_WRITE_STATUS;
    Status = (*Fvb)->SetAttributes (*Fvb, &Protected);
  }

  if (EFI_ERROR (Status)) {
    *Fvb = NULL;
  }

  return Status;
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
        // Never install another board's training cache from a saved SPI image.
        if ((Offset >= K3_DDR_TRAINING_OFFSET) && (Offset < K3_DDR_TRAINING_OFFSET + SIZE_64KB)) {
          Status = EFI_SUCCESS;
        } else {
          Status = Nor->WriteData (Nor, Offset, Step, (UINT8 *)Image + Offset);
        }
      } else {
        Status = Nor->ReadData (Nor, Offset, Step, Scratch);
        CONST UINT8  *Expected = InNv (Offset) ? Nv + Offset - K3_FIRMWARE_NV_OFFSET
                                             : Image + Offset;
        if (!EFI_ERROR (Status)) {
          if ((Offset >= K3_DDR_TRAINING_OFFSET) && (Offset < K3_DDR_TRAINING_OFFSET + SIZE_64KB)) {
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
