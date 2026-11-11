/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/K3CpuConfigLib.h>
#include <Library/K3SpiNorLib.h>

BOOLEAN
K3CpuConfigValid (CONST K3_CPU_CONFIG_RECORD *Record)
{
  return Record != NULL && Record->Magic == K3_CPU_CONFIG_MAGIC &&
         Record->Version == 1 && Record->Size == sizeof (*Record) &&
         Record->Mode <= K3CpuA100Only &&
         Record->Crc32 == CalculateCrc32 ((VOID *)Record, OFFSET_OF (K3_CPU_CONFIG_RECORD, Crc32));
}

EFI_STATUS
K3CpuConfigGet (UINT32 *Mode)
{
  EFI_SPI_NOR_FLASH_PROTOCOL *Nor;
  K3_CPU_CONFIG_RECORD Record;
  EFI_STATUS Status;
  if (Mode == NULL) { return EFI_INVALID_PARAMETER; }
  *Mode = K3CpuX100Only;
  Status = K3SpiNorFindFlash (&Nor);
  if (EFI_ERROR (Status)) { return Status; }
  if (Nor->FlashSize != SIZE_8MB) { return EFI_UNSUPPORTED; }
  Status = Nor->ReadData (Nor, K3_CPU_CONFIG_OFFSET, sizeof (Record), (UINT8 *)&Record);
  if (!EFI_ERROR (Status) && K3CpuConfigValid (&Record)) { *Mode = Record.Mode; }
  return Status;
}

EFI_STATUS
K3CpuConfigSet (UINT32 Mode)
{
  EFI_SPI_NOR_FLASH_PROTOCOL *Nor;
  K3_CPU_CONFIG_RECORD Record, Verify;
  EFI_STATUS Status;
  UINT32 Current;
  if (Mode > K3CpuA100Only) { return EFI_INVALID_PARAMETER; }
  Status = K3CpuConfigGet (&Current);
  if (EFI_ERROR (Status) || Current == Mode) { return Status; }
  Status = K3SpiNorFindFlash (&Nor);
  if (EFI_ERROR (Status)) { return Status; }
  if (Nor->EraseBlockBytes != SIZE_4KB) { return EFI_UNSUPPORTED; }
  Record = (K3_CPU_CONFIG_RECORD) {K3_CPU_CONFIG_MAGIC, 1, sizeof (Record), Mode, 0};
  Record.Crc32 = CalculateCrc32 (&Record, OFFSET_OF (K3_CPU_CONFIG_RECORD, Crc32));
  Status = Nor->Erase (Nor, K3_CPU_CONFIG_OFFSET, 1);
  if (!EFI_ERROR (Status)) { Status = Nor->WriteData (Nor, K3_CPU_CONFIG_OFFSET, sizeof (Record), (UINT8 *)&Record); }
  if (!EFI_ERROR (Status)) { Status = Nor->ReadData (Nor, K3_CPU_CONFIG_OFFSET, sizeof (Verify), (UINT8 *)&Verify); }
  if (!EFI_ERROR (Status) && CompareMem (&Record, &Verify, sizeof (Record))) { Status = EFI_DEVICE_ERROR; }
  return Status;
}
