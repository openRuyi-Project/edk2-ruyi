/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/K3CpuConfigLib.h>
#include <Library/K3FirmwareUpdateLib.h>

STATIC EFI_STATUS
ReadConfig (
  EFI_SPI_NOR_FLASH_PROTOCOL  **Nor,
  UINT32                      *Mode
  )
{
  K3_CPU_CONFIG_RECORD  Record;
  EFI_STATUS            Status;

  *Mode  = K3CpuX100Only;
  Status = K3FirmwareFindFlash (Nor);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((*Nor)->FlashSize != K3_FIRMWARE_SIZE) {
    return EFI_UNSUPPORTED;
  }

  Status = (*Nor)->ReadData (*Nor, K3_CPU_CONFIG_OFFSET, sizeof (Record), (UINT8 *)&Record);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Record.Magic == K3_CPU_CONFIG_MAGIC) &&
      (Record.Version == 1) && (Record.Size == sizeof (Record)) &&
      (Record.Mode <= K3CpuA100Only) &&
      (Record.Crc32 == CalculateCrc32 (&Record, OFFSET_OF (K3_CPU_CONFIG_RECORD, Crc32))))
  {
    *Mode = Record.Mode;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
K3CpuConfigGet (
  UINT32  *Mode
  )
{
  EFI_SPI_NOR_FLASH_PROTOCOL  *Nor;

  if (Mode == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return ReadConfig (&Nor, Mode);
}

EFI_STATUS
K3CpuConfigSet (
  UINT32  Mode
  )
{
  EFI_SPI_NOR_FLASH_PROTOCOL  *Nor;
  K3_CPU_CONFIG_RECORD        Record, Verify;
  UINT32                      Current;
  EFI_STATUS                  Status;

  if (Mode > K3CpuA100Only) {
    return EFI_INVALID_PARAMETER;
  }

  Status = ReadConfig (&Nor, &Current);
  if (EFI_ERROR (Status) || (Current == Mode)) {
    return Status;
  }

  // Do not erase neighboring firmware on devices without 4 KiB erase support.
  if (Nor->EraseBlockBytes != K3_CPU_CONFIG_SIZE) {
    return EFI_UNSUPPORTED;
  }

  SetMem (&Record, sizeof (Record), 0xff);
  if (Mode != K3CpuX100Only) {
    Record.Magic   = K3_CPU_CONFIG_MAGIC;
    Record.Version = 1;
    Record.Size    = sizeof (Record);
    Record.Mode    = Mode;
    Record.Crc32   = CalculateCrc32 (&Record, OFFSET_OF (K3_CPU_CONFIG_RECORD, Crc32));
  }

  Status = Nor->Erase (Nor, K3_CPU_CONFIG_OFFSET, 1);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Mode != K3CpuX100Only) {
    Status = Nor->WriteData (Nor, K3_CPU_CONFIG_OFFSET, sizeof (Record), (UINT8 *)&Record);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = Nor->ReadData (Nor, K3_CPU_CONFIG_OFFSET, sizeof (Verify), (UINT8 *)&Verify);
  if (!EFI_ERROR (Status) && CompareMem (&Record, &Verify, sizeof (Record))) {
    Status = EFI_DEVICE_ERROR;
  }

  return Status;
}
