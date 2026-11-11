/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PlatformSettingsLib.h>
BOOLEAN PlatformSettingsRecordValid (CONST RUYI_SETTINGS_RECORD *Record)
{
  return Record->Magic == SETTINGS_RECORD_MAGIC && Record->Version == 1 &&
         Record->Size == sizeof (*Record) && !(Record->Values & ~RUYI_SETTINGS_MASK) &&
         Record->Commit == SETTINGS_RECORD_COMMIT &&
         Record->Crc32 == CalculateCrc32 ((VOID *)Record, OFFSET_OF (RUYI_SETTINGS_RECORD, Crc32));
}
INTN PlatformSettingsNewest (CONST RUYI_SETTINGS_RECORD Records[SETTINGS_SLOT_COUNT])
{
  INTN Selected = -1;
  for (UINTN Slot = 0; Slot < SETTINGS_SLOT_COUNT; Slot++) {
    if (PlatformSettingsRecordValid (&Records[Slot]) &&
        (Selected < 0 || (INT32)(Records[Slot].Generation - Records[Selected].Generation) > 0)) {
      Selected = (INTN)Slot;
    }
  }
  return Selected;
}
EFI_STATUS PlatformSettingsImageValid (CONST VOID *Image, UINTN Size, UINT32 Offset, UINT32 Stride)
{
  RUYI_SETTINGS_RECORD Records[SETTINGS_SLOT_COUNT];
  if (Image == NULL || Stride < sizeof (*Records)) { return EFI_INVALID_PARAMETER; }
  for (UINTN Slot = 0; Slot < SETTINGS_SLOT_COUNT; Slot++) {
    UINT64 Address = (UINT64)Offset + Slot * (UINT64)Stride;
    if (Address > Size || sizeof (*Records) > Size - Address) { return EFI_BAD_BUFFER_SIZE; }
    CopyMem (&Records[Slot], (CONST UINT8 *)Image + Address, sizeof (*Records));
  }
  return PlatformSettingsNewest (Records) < 0 ? EFI_COMPROMISED_DATA : EFI_SUCCESS;
}

EFI_STATUS PlatformSettingsSnapshot (RUYI_SETTINGS_RECORD *Record)
{
  RUYI_SETTINGS_RECORD Records[SETTINGS_SLOT_COUNT];
  INTN Selected;
  if (Record == NULL) { return EFI_INVALID_PARAMETER; }
  for (UINTN Slot = 0; Slot < SETTINGS_SLOT_COUNT; Slot++) {
    EFI_STATUS Status = PlatformSettingsStorageRead (Slot, 0, sizeof (*Records), &Records[Slot]);
    if (EFI_ERROR (Status)) { return Status; }
  }
  Selected = PlatformSettingsNewest (Records);
  if (Selected < 0) { return EFI_NOT_FOUND; }
  *Record = Records[Selected];
  return EFI_SUCCESS;
}
