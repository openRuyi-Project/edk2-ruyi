/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/K3SpiNorLib.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

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
K3SpiNorFindFlash (
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
