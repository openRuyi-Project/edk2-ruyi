/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PlatformSettingsLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "Defaults.h"

STATIC BOOLEAN mBusy;

STATIC EFI_STATUS
Read (RUYI_SETTINGS_RECORD Records[SETTINGS_SLOT_COUNT], INTN *Selected)
{
  for (UINTN Slot = 0; Slot < SETTINGS_SLOT_COUNT; Slot++) {
    EFI_STATUS Status = PlatformSettingsStorageRead (Slot, 0, sizeof (*Records), &Records[Slot]);
    if (EFI_ERROR (Status)) { return Status; }
  }
  *Selected = PlatformSettingsNewest (Records);
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
GetDefaults (UINT32 *Values)
{
  RUYI_SETTINGS_RECORD Records[SETTINGS_SLOT_COUNT];
  INTN Selected;
  EFI_STATUS Status;
  EFI_TPL Old;
  if (Values == NULL) { return EFI_INVALID_PARAMETER; }
  Old = gBS->RaiseTPL (TPL_NOTIFY);
  if (mBusy) { gBS->RestoreTPL (Old); return EFI_NOT_READY; }
  mBusy = TRUE;
  Status = Read (Records, &Selected);
  if (!EFI_ERROR (Status)) {
    if (Selected < 0) { Status = EFI_NOT_FOUND; }
    else { *Values = Records[Selected].Values; }
  }
  mBusy = FALSE;
  gBS->RestoreTPL (Old);
  return Status;
}

EFI_STATUS EFIAPI
SetDefaults (UINT32 Values)
{
  RUYI_SETTINGS_RECORD Records[SETTINGS_SLOT_COUNT], Next, Verify;
  INTN Selected;
  UINTN Slot;
  EFI_STATUS Status;
  EFI_TPL Old;
  if (Values & ~RUYI_SETTINGS_MASK) { return EFI_INVALID_PARAMETER; }
  Old = gBS->RaiseTPL (TPL_NOTIFY);
  if (mBusy) { gBS->RestoreTPL (Old); return EFI_NOT_READY; }
  mBusy = TRUE;
  Status = Read (Records, &Selected);
  if (EFI_ERROR (Status) || (Selected >= 0 && Records[Selected].Values == Values)) { goto Done; }
  Slot = Selected < 0 ? 0 : (UINTN)Selected ^ 1;
  Next = (RUYI_SETTINGS_RECORD) {SETTINGS_RECORD_MAGIC, SETTINGS_RECORD_VERSION, sizeof (Next),
    Selected < 0 ? 0 : Records[Selected].Generation + 1, Values, 0, SETTINGS_RECORD_COMMIT};
  Next.Crc32 = CalculateCrc32 (&Next, OFFSET_OF (RUYI_SETTINGS_RECORD, Crc32));
  Status = PlatformSettingsStoragePrepare (Slot);
  if (!EFI_ERROR (Status)) { Status = PlatformSettingsStorageWrite (Slot, 0, OFFSET_OF (RUYI_SETTINGS_RECORD, Commit), &Next); }
  if (!EFI_ERROR (Status)) { Status = PlatformSettingsStorageRead (Slot, 0, sizeof (Verify), &Verify); }
  if (!EFI_ERROR (Status) && (CompareMem (&Next, &Verify, OFFSET_OF (RUYI_SETTINGS_RECORD, Commit)) || Verify.Commit != MAX_UINT32)) {
    Status = EFI_DEVICE_ERROR;
  }
  if (!EFI_ERROR (Status)) { Status = PlatformSettingsStorageWrite (Slot, OFFSET_OF (RUYI_SETTINGS_RECORD, Commit), sizeof (Next.Commit), &Next.Commit); }
  if (!EFI_ERROR (Status)) { Status = PlatformSettingsStorageRead (Slot, 0, sizeof (Verify), &Verify); }
  if (!EFI_ERROR (Status) && (!PlatformSettingsRecordValid (&Verify) || CompareMem (&Verify, &Next, sizeof (Next)))) { Status = EFI_DEVICE_ERROR; }
Done:
  mBusy = FALSE;
  gBS->RestoreTPL (Old);
  return Status;
}
