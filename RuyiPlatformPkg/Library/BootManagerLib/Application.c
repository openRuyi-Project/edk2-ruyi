// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Protocol/FirmwareVolume2.h>
#include <Library/BootManagerLib.h>
#include <Library/BootProgressLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

EFI_STATUS EFIAPI
BootManagerRegisterFvApplication (
  CONST EFI_GUID  *Guid,
  CHAR16          *Title,
  UINT32          Attributes,
  EFI_INPUT_KEY   *Key OPTIONAL,
  BOOLEAN         Launch
  )
{
  EFI_DEVICE_PATH_PROTOCOL           *Path;
  EFI_BOOT_MANAGER_LOAD_OPTION       Option;
  EFI_BOOT_MANAGER_LOAD_OPTION       *Existing;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  File;
  EFI_STATUS                         Status;
  EFI_STATUS                         KeyStatus = EFI_SUCCESS;
  EFI_HANDLE                         *Handles;
  UINTN                              Count, Index, FileSize;
  UINTN                              ExistingCount;
  EFI_FIRMWARE_VOLUME2_PROTOCOL      *Fv;
  EFI_FV_FILETYPE                    FileType;
  EFI_FV_FILE_ATTRIBUTES             FileAttributes;
  UINT32                             Authentication;

  if ((Guid == NULL) || (Title == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  EfiInitializeFwVolDevicepathNode (&File, Guid);
  Path   = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&Fv
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    FileSize = 0;
    Status   = Fv->ReadFile (
                     Fv,
                     Guid,
                     NULL,
                     &FileSize,
                     &FileType,
                     &FileAttributes,
                     &Authentication
                     );
    if (!EFI_ERROR (Status) && (FileType == EFI_FV_FILETYPE_APPLICATION)) {
      EFI_DEVICE_PATH_PROTOCOL  *Parent = DevicePathFromHandle (Handles[Index]);
      if (Parent == NULL) {
        continue;
      }

      Path = AppendDevicePathNode (Parent, &File.Header);
      if (Path == NULL) {
        FreePool (Handles);
        return EFI_OUT_OF_RESOURCES;
      }

      break;
    }
  }

  FreePool (Handles);
  if (Path == NULL) {
    return EFI_NOT_FOUND;
  }

  Status = EfiBootManagerInitializeLoadOption (
             &Option,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             Attributes,
             Title,
             Path,
             NULL,
             0
             );
  if (!EFI_ERROR (Status)) {
    Existing = EfiBootManagerGetLoadOptions (&ExistingCount, LoadOptionTypeBoot);
    INTN  Match = EfiBootManagerFindLoadOption (&Option, Existing, ExistingCount);
    if (Match == -1) {
      Status = EfiBootManagerAddLoadOptionVariable (&Option, MAX_UINTN);
    } else {
      Option.OptionNumber = Existing[Match].OptionNumber;
    }

    if (!EFI_ERROR (Status) && (Key != NULL)) {
      KeyStatus = EfiBootManagerAddKeyOptionVariable (
                    NULL,
                    (UINT16)Option.OptionNumber,
                    0,
                    Key,
                    NULL
                    );
      if (EFI_ERROR (KeyStatus) && (KeyStatus != EFI_ALREADY_STARTED)) {
        DEBUG ((DEBUG_WARN, "%s hotkey: %r\n", Title, KeyStatus));
      }
    }

    if (Existing != NULL) {
      EfiBootManagerFreeLoadOptions (Existing, ExistingCount);
    }

    DEBUG ((DEBUG_INFO, "%s boot option: %r\n", Title, Status));
    // Recovery must start even when its hotkey already exists.
    if (!EFI_ERROR (Status) && Launch) {
      BootProgressStop ();
      EfiBootManagerBoot (&Option);
    }

    EfiBootManagerFreeLoadOption (&Option);
    if (!EFI_ERROR (Status)) {
      Status = KeyStatus;
    }
  }

  FreePool (Path);
  return Status;
}
