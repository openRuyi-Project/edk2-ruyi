/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/K3DdrTraining.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/K3FirmwareUpdateLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_STATUS EFIAPI
K3DdrTrainingEntry (
  EFI_HANDLE        ImageHandle,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE           *Hob;
  K3_DDR_TRAINING_RECORD      *Record;
  EFI_SPI_NOR_FLASH_PROTOCOL  *Nor;
  EFI_STATUS                  Status;
  UINT8                       *Verify;
  UINT32                      EraseBytes;

  // Use the actual BootROM boot mode, not the board's NOR boot straps.
  // A normal payload reached through USB recovery must not save a record.
  if ((MmioRead32 (0xd4282d10) & 0xfff) != 0xb01) {
    return EFI_SUCCESS;
  }

  Hob = GetFirstGuidHob (&gSpacemitDdrTrainingGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) != sizeof (*Record))) {
    return EFI_SUCCESS;
  }

  Record = GET_GUID_HOB_DATA (Hob);
  if ((Record->Magic != K3_DDR_TRAINING_MAGIC) ||
      (Record->Crc32 != CalculateCrc32 (&Record->ChipId, sizeof (*Record) - 8)))
  {
    return EFI_COMPROMISED_DATA;
  }

  // The platform publishes its boot flash through the standard NOR protocol.
  Status = gBS->LocateProtocol (&gEfiSpiNorFlashProtocolGuid, NULL, (VOID **)&Nor);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // The original NOR private area is the second 64 KiB of BOOTINFO.
  if ((Nor->FlashSize != K3_FIRMWARE_SIZE) || (Nor->EraseBlockBytes > SIZE_64KB) ||
      (SIZE_64KB % Nor->EraseBlockBytes != 0))
  {
    return EFI_UNSUPPORTED;
  }

  Verify = AllocatePool (sizeof (*Record));
  if (Verify == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  EraseBytes = ALIGN_VALUE (sizeof (*Record), Nor->EraseBlockBytes);
  Status     = Nor->Erase (Nor, K3_DDR_TRAINING_OFFSET, EraseBytes / SIZE_4KB);
  if (!EFI_ERROR (Status)) {
    Status = Nor->WriteData (Nor, K3_DDR_TRAINING_OFFSET, sizeof (*Record), (UINT8 *)Record);
  }

  if (!EFI_ERROR (Status)) {
    Status = Nor->ReadData (Nor, K3_DDR_TRAINING_OFFSET, sizeof (*Record), Verify);
    if (!EFI_ERROR (Status) && CompareMem (Verify, Record, sizeof (*Record))) {
      Status = EFI_CRC_ERROR;
    }
  }

  FreePool (Verify);
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO, "DDR training record save: %r\n", Status));
  return Status;
}
