/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PlatformSettingsLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/Variable.h>
#include <Protocol/VariableWrite.h>

STATIC BOOLEAN mBusy, mPublished;
STATIC EFI_HANDLE mHandle;
STATIC EFI_EVENT mVariableEvent, mEndEvent;
STATIC VOID *mRegistration;

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

STATIC EFI_STATUS EFIAPI
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

STATIC EFI_STATUS EFIAPI
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

STATIC EFI_STATUS EFIAPI
GetEffective (UINT32 *Values, BOOLEAN *Present)
{
  RUYI_SETTINGS_OVERRIDE Override;
  UINTN Size = sizeof (Override);
  EFI_STATUS Status;
  if (Present != NULL) { *Present = FALSE; }
  Status = GetDefaults (Values);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gRT->GetVariable (RUYI_SETTINGS_OVERRIDE_NAME, &gRuyiPlatformSettingsProtocolGuid,
                            NULL, &Size, &Override);
  if (Status == EFI_NOT_FOUND || Status == EFI_UNSUPPORTED || Status == EFI_NOT_AVAILABLE_YET) { return EFI_SUCCESS; }
  if (Present != NULL && (Status == EFI_SUCCESS || Status == EFI_BUFFER_TOO_SMALL)) { *Present = TRUE; }
  if (Status == EFI_BUFFER_TOO_SMALL) { return EFI_COMPROMISED_DATA; }
  if (EFI_ERROR (Status)) { return Status; }
  if (Size != sizeof (Override) || (Override.Mask & ~RUYI_SETTINGS_MASK) || (Override.Values & ~Override.Mask)) {
    return EFI_COMPROMISED_DATA;
  }
  *Values = (*Values & ~Override.Mask) | Override.Values;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
ClearOverrides (VOID)
{
  EFI_STATUS Status = gRT->SetVariable (RUYI_SETTINGS_OVERRIDE_NAME,
                       &gRuyiPlatformSettingsProtocolGuid, 0, 0, NULL);
  return Status == EFI_NOT_FOUND ? EFI_SUCCESS : Status;
}

STATIC RUYI_PLATFORM_SETTINGS_PROTOCOL mProtocol = {GetDefaults, SetDefaults, GetEffective, ClearOverrides};

STATIC VOID EFIAPI
Publish (EFI_EVENT Event, VOID *Context)
{
  UINT32 Values = 0;
  VOID *Variable;
  EFI_STATUS Status;
  if (mPublished) { return; }
  // Use variables when present. EndOfDxe also releases boards without them.
  if (Context == NULL &&
      (EFI_ERROR (gBS->LocateProtocol (&gEfiVariableArchProtocolGuid, NULL, &Variable)) ||
       EFI_ERROR (gBS->LocateProtocol (&gEfiVariableWriteArchProtocolGuid, NULL, &Variable)))) { return; }
  Status = GetEffective (&Values, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "Platform defaults: %r\n", Status));
    Values = 0;
  }
  Status = PcdSetBoolS (PcdMauEmulationEnabled,
             FixedPcdGetBool (PcdDriverEmulationAvailable) && !!(Values & RUYI_SETTING_EMULATION));
  if (EFI_ERROR (Status)) { return; }
  Status = gBS->InstallProtocolInterface (&mHandle, &gRuyiPlatformSettingsProtocolGuid,
                                        EFI_NATIVE_INTERFACE, &mProtocol);
  if (!EFI_ERROR (Status)) { mPublished = TRUE; }
}

EFI_STATUS EFIAPI
PlatformSettingsEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Publish, (VOID *)1,
                                         &gEfiEndOfDxeEventGroupGuid, &mEndEvent);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Publish, NULL, &mVariableEvent);
  if (!EFI_ERROR (Status)) { Status = gBS->RegisterProtocolNotify (&gEfiVariableWriteArchProtocolGuid, mVariableEvent, &mRegistration); }
  if (EFI_ERROR (Status)) {
    if (mVariableEvent) { gBS->CloseEvent (mVariableEvent); }
    gBS->CloseEvent (mEndEvent);
    return Status;
  }
  Publish (NULL, NULL);
  return EFI_SUCCESS;
}
