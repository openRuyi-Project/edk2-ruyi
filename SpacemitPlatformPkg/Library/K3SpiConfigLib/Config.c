/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/K3CpuConfigLib.h>
#include <Library/K3SpiConfigLib.h>
#include <Library/K3FirmwareUpdateLib.h>
#include <Library/K3SpiNorLib.h>

typedef struct {
  UINT32    Magic;
  UINT16    Version;
  UINT16    Size;
  UINT32    Value;
  UINT32    Crc32;
} K3_SPI_CONFIG_RECORD;

typedef struct {
  UINT32    Offset;
  UINT32    Magic;
  UINT32    Maximum;
} K3_SPI_CONFIG;

STATIC CONST K3_SPI_CONFIG  mSettings[] = {
  { K3_CPU_CONFIG_OFFSET,       K3_CPU_CONFIG_MAGIC, K3CpuA100Only },
  { K3_EMULATION_CONFIG_OFFSET, SIGNATURE_32 ('K',   '3', 'E', 'M'), 1}
};

STATIC EFI_STATUS
ReadConfig (
  CONST K3_SPI_CONFIG         *Config,
  EFI_SPI_NOR_FLASH_PROTOCOL  **Nor,
  UINT32                      *Value
  )
{
  K3_SPI_CONFIG_RECORD  Record;
  EFI_STATUS            Status;

  *Value = 0;
  Status = K3SpiNorFindFlash (Nor);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((*Nor)->FlashSize != K3_FIRMWARE_SIZE) {
    return EFI_UNSUPPORTED;
  }

  Status = (*Nor)->ReadData (*Nor, Config->Offset, sizeof (Record), (UINT8 *)&Record);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Record.Magic == Config->Magic) &&
      (Record.Version == 1) && (Record.Size == sizeof (Record)) &&
      (Record.Value <= Config->Maximum) &&
      (Record.Crc32 == CalculateCrc32 (&Record, OFFSET_OF (K3_SPI_CONFIG_RECORD, Crc32))))
  {
    *Value = Record.Value;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
K3SpiConfigGet (
  K3_SPI_SETTING  Setting,
  UINT32          *Value
  )
{
  EFI_SPI_NOR_FLASH_PROTOCOL  *Nor;

  if ((Value == NULL) || ((UINTN)Setting >= ARRAY_SIZE (mSettings))) {
    return EFI_INVALID_PARAMETER;
  }

  return ReadConfig (&mSettings[Setting], &Nor, Value);
}

EFI_STATUS
K3SpiConfigSet (
  K3_SPI_SETTING  Setting,
  UINT32          Value
  )
{
  CONST K3_SPI_CONFIG         *Config;
  EFI_SPI_NOR_FLASH_PROTOCOL  *Nor;
  K3_SPI_CONFIG_RECORD        Record, Verify;
  UINT32                      Current;
  EFI_STATUS                  Status;

  if (((UINTN)Setting >= ARRAY_SIZE (mSettings)) || (Value > mSettings[Setting].Maximum)) {
    return EFI_INVALID_PARAMETER;
  }

  Config = &mSettings[Setting];
  Status = ReadConfig (Config, &Nor, &Current);
  if (EFI_ERROR (Status) || (Current == Value)) {
    return Status;
  }

  // Do not erase neighboring firmware on devices without 4 KiB erase support.
  if (Nor->EraseBlockBytes != SIZE_4KB) {
    return EFI_UNSUPPORTED;
  }

  SetMem (&Record, sizeof (Record), 0xff);
  if (Value != 0) {
    Record.Magic   = Config->Magic;
    Record.Version = 1;
    Record.Size    = sizeof (Record);
    Record.Value   = Value;
    Record.Crc32   = CalculateCrc32 (&Record, OFFSET_OF (K3_SPI_CONFIG_RECORD, Crc32));
  }

  Status = Nor->Erase (Nor, Config->Offset, 1);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Value != 0) {
    Status = Nor->WriteData (Nor, Config->Offset, sizeof (Record), (UINT8 *)&Record);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = Nor->ReadData (Nor, Config->Offset, sizeof (Verify), (UINT8 *)&Verify);
  if (!EFI_ERROR (Status) && CompareMem (&Record, &Verify, sizeof (Record))) {
    Status = EFI_DEVICE_ERROR;
  }

  return Status;
}
