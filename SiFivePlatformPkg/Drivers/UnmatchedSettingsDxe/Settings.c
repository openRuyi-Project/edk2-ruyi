/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/UnmatchedSettings.h>
#include "Record.h"

STATIC SETTINGS_RECORD  mRecord;
STATIC INTN             mSlot = -1;
STATIC BOOLEAN          mBusy;

STATIC BOOLEAN
Valid (
  CONST SETTINGS_RECORD  *Record
  )
{
  return Record->Magic == SETTINGS_MAGIC && Record->Version == 1 &&
         Record->Size == sizeof (*Record) && Record->Flags <= 3 &&
         Record->Commit == SETTINGS_COMMIT &&
         Record->Crc32 == CalculateCrc32 ((VOID *)Record, OFFSET_OF (SETTINGS_RECORD, Crc32));
}

STATIC EFI_STATUS
ReadSettings (
  VOID
  )
{
  SETTINGS_RECORD  Records[2];
  EFI_STATUS       Status;

  mSlot = -1;
  for (UINTN Index = 0; Index < ARRAY_SIZE (Records); Index++) {
    Status = SettingsFlashRead (
               SETTINGS_OFFSET + Index * SETTINGS_SECTOR_SIZE,
               sizeof (Records[Index]),
               &Records[Index]
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Valid (&Records[Index]) &&
        ((mSlot < 0) || ((INT32)(Records[Index].Generation - mRecord.Generation) > 0)))
    {
      mSlot   = (INTN)Index;
      mRecord = Records[Index];
    }
  }

  if (mSlot < 0) {
    ZeroMem (&mRecord, sizeof (mRecord));
    mRecord.Flags = FixedPcdGetBool (PcdUnmatchedDefaultEmulation) ? 3 : 0;
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
Get (
  BOOLEAN  *Enabled,
  BOOLEAN  *DefaultEnabled
  )
{
  EFI_TPL     Tpl;
  EFI_STATUS  Status;

  if (!Enabled || !DefaultEnabled) {
    return EFI_INVALID_PARAMETER;
  }

  Tpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mBusy) {
    gBS->RestoreTPL (Tpl);
    return EFI_NOT_READY;
  }

  mBusy  = TRUE;
  Status = ReadSettings ();
  if (!EFI_ERROR (Status)) {
    *Enabled        = (mRecord.Flags & SETTINGS_ENABLED) != 0;
    *DefaultEnabled = (mRecord.Flags & SETTINGS_DEFAULT_ENABLED) != 0;
  }

  mBusy = FALSE;
  gBS->RestoreTPL (Tpl);
  return Status;
}

STATIC EFI_STATUS EFIAPI
Set (
  BOOLEAN  Enabled
  )
{
  SETTINGS_RECORD  Record, Verify;
  UINTN            Slot;
  UINT32           Offset;
  EFI_STATUS       Status;
  EFI_TPL          Tpl;

  if ((Enabled > 1) || (Enabled && !FixedPcdGetBool (PcdUnmatchedEmulationAvailable))) {
    return EFI_UNSUPPORTED;
  }

  Tpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mBusy) {
    gBS->RestoreTPL (Tpl);
    return EFI_NOT_READY;
  }

  mBusy  = TRUE;
  Status = ReadSettings ();
  if (EFI_ERROR (Status) || (((mRecord.Flags & SETTINGS_ENABLED) != 0) == Enabled)) {
    mBusy = FALSE;
    gBS->RestoreTPL (Tpl);
    return Status;
  }

  Record.Magic      = SETTINGS_MAGIC;
  Record.Version    = 1;
  Record.Size       = sizeof (Record);
  Record.Generation = mSlot < 0 ? 0 : mRecord.Generation + 1;
  Record.Flags      = (mRecord.Flags & SETTINGS_DEFAULT_ENABLED) | (Enabled ? SETTINGS_ENABLED : 0);
  Record.Crc32      = CalculateCrc32 (&Record, OFFSET_OF (SETTINGS_RECORD, Crc32));
  Record.Commit     = SETTINGS_COMMIT;
  Slot              = mSlot == 0 ? 1 : 0;
  Offset            = SETTINGS_OFFSET + Slot * SETTINGS_SECTOR_SIZE;

  // Keep the previous sector valid until the new record is verified and committed.
  Status = SettingsFlashErase (Offset);
  if (!EFI_ERROR (Status)) {
    Status = SettingsFlashWrite (Offset, OFFSET_OF (SETTINGS_RECORD, Commit), &Record);
  }

  if (!EFI_ERROR (Status)) {
    Status = SettingsFlashRead (Offset, sizeof (Verify), &Verify);
  }

  if (!EFI_ERROR (Status) &&
      (CompareMem (&Record, &Verify, OFFSET_OF (SETTINGS_RECORD, Commit)) || (Verify.Commit != MAX_UINT32)))
  {
    Status = EFI_DEVICE_ERROR;
  }

  if (!EFI_ERROR (Status)) {
    Status = SettingsFlashWrite (
               Offset + OFFSET_OF (SETTINGS_RECORD, Commit),
               sizeof (Record.Commit),
               &Record.Commit
               );
  }

  if (!EFI_ERROR (Status)) {
    Status = SettingsFlashRead (Offset, sizeof (Verify), &Verify);
  }

  if (!EFI_ERROR (Status) && (!Valid (&Verify) || CompareMem (&Record, &Verify, sizeof (Record)))) {
    Status = EFI_DEVICE_ERROR;
  }

  if (!EFI_ERROR (Status)) {
    mRecord = Record;
    mSlot   = (INTN)Slot;
  }

  mBusy = FALSE;
  gBS->RestoreTPL (Tpl);
  return Status;
}

EFI_STATUS EFIAPI
UnmatchedSettingsEntry (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  STATIC UNMATCHED_SETTINGS_PROTOCOL  Protocol = { Get, Set };
  EFI_HANDLE                          Handle   = NULL;
  EFI_STATUS                          Status   = SettingsFlashInit ();

  if (!EFI_ERROR (Status)) {
    Status = ReadSettings ();
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Unmatched settings SPI: %r\n", Status));
    return Status;
  }

  Status = PcdSetBoolS (
             PcdMauEmulationEnabled,
             FixedPcdGetBool (PcdUnmatchedEmulationAvailable) &&
             ((mRecord.Flags & SETTINGS_ENABLED) != 0)
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
          "Unmatched driver emulation: %a (%a)\n",
          PcdGetBool (PcdMauEmulationEnabled) ? "enabled" : "disabled",
          mSlot >= 0 ? "SPI" : "build default"
    ));
  return gBS->InstallProtocolInterface (
                &Handle,
                &gUnmatchedSettingsProtocolGuid,
                EFI_NATIVE_INTERFACE,
                &Protocol
                );
}
