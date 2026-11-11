/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Protocol/DevicePath.h>
#include <Protocol/HiiConfigAccess.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FileExplorerLib.h>
#include <Library/FileHandleLib.h>
#include <Library/HiiLib.h>
#include <Library/PlatformFirmwareUpdateLib.h>
#include <Library/BaseLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include "Forms.h"
#include "FirmwareUpdateUiLibStrDefs.h"

extern UINT8           FormsBin[];
extern UINT8           FirmwareUpdateUiLibStrings[];
STATIC EFI_GUID        mFormGuid = FIRMWARE_UPDATE_FORMSET_GUID;
STATIC EFI_HANDLE      mHandle;
STATIC EFI_HII_HANDLE  mHiiHandle;
STATIC BOOLEAN         mFileSelected;
STATIC struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} mPath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,                   { sizeof (VENDOR_DEVICE_PATH),       0 }
    }, FIRMWARE_UPDATE_FORMSET_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

typedef struct {
  FIRMWARE_UPDATE_PHASE    Phase;
  UINTN                    Percent;
  UINTN                    Row;
} UPDATE_PROGRESS;

STATIC EFI_STATUS
Progress (
  FIRMWARE_UPDATE_PHASE  Phase,
  UINT64                 Done,
  UINT64                 Total,
  VOID                   *Context
  )
{
  STATIC CONST CHAR16  *Names[] = { L"Reading", L"Erasing", L"Writing", L"Verifying" };
  UPDATE_PROGRESS      *State   = Context;
  UINTN                Percent  = Total ? (UINTN)(Done * 100 / Total) : 0;

  if (Phase >= ARRAY_SIZE (Names)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Phase != State->Phase) || (Percent != State->Percent)) {
    gST->ConOut->SetCursorPosition (gST->ConOut, 0, State->Row);
    Print (L"%-20s %3u%%     ", Names[Phase], Percent);
    State->Phase   = Phase;
    State->Percent = Percent;
  }

  return EFI_SUCCESS;
}

// ChooseFile returns a filesystem device path followed by one full file path.
STATIC EFI_STATUS
CheckFileChecksum (
  EFI_DEVICE_PATH_PROTOCOL  *ImagePath,
  CONST CHAR16              *ImageName,
  CONST VOID                *Image,
  UINTN                     ImageSize
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Cursor = ImagePath, *Path = NULL;
  EFI_HANDLE                Device;
  EFI_FILE_PROTOCOL         *File = NULL;
  EFI_FILE_INFO             *Info = NULL;
  CHAR16                    *Name = NULL;
  CHAR8                     *Data = NULL;
  EFI_STATUS                Status;
  UINTN                     Prefix = 0, Size;

  Status = gBS->LocateDevicePath (&gEfiSimpleFileSystemProtocolGuid, &Cursor, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((DevicePathType (Cursor) != MEDIA_DEVICE_PATH) ||
      (DevicePathSubType (Cursor) != MEDIA_FILEPATH_DP) ||
      (DevicePathNodeLength (Cursor) < sizeof (FILEPATH_DEVICE_PATH)) ||
      !IsDevicePathEnd (NextDevicePathNode (Cursor)))
  {
    return EFI_UNSUPPORTED;
  }

  Size = DevicePathNodeLength (Cursor) - sizeof (EFI_DEVICE_PATH_PROTOCOL);
  CONST CHAR16  *PathName = ((FILEPATH_DEVICE_PATH *)Cursor)->PathName;
  CHAR16        *Selected = AllocateCopyPool (Size, PathName);

  if (!Selected) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (Size % sizeof (CHAR16) || Selected[Size / sizeof (CHAR16) - 1]) {
    FreePool (Selected);
    return EFI_COMPROMISED_DATA;
  }

  for (UINTN I = 0; Selected[I]; I++) {
    if (Selected[I] == L'\\') {
      Prefix = I + 1;
    }
  }

  Name = AllocatePool (Prefix * sizeof (CHAR16) + sizeof (L"SHA256SUMS"));
  if (!Name) {
    FreePool (Selected);
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Name, Selected, Prefix * sizeof (CHAR16));
  FreePool (Selected);
  CopyMem (Name + Prefix, L"SHA256SUMS", sizeof (L"SHA256SUMS"));
  Path = FileDevicePath (Device, Name);
  FreePool (Name);
  if (!Path) {
    return EFI_OUT_OF_RESOURCES;
  }

  Cursor = Path;
  Status = EfiOpenFileByDevicePath (&Cursor, &File, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    goto Out;
  }

  Info = FileHandleGetInfo (File);
  if (!Info) {
    Status = EFI_DEVICE_ERROR;
    goto Out;
  }

  if ((Info->Attribute & EFI_FILE_DIRECTORY) || !Info->FileSize || (Info->FileSize > SIZE_64KB)) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Out;
  }

  Size = (UINTN)Info->FileSize;
  Data = AllocatePool (Size);
  if (!Data) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Out;
  }

  Status = File->Read (File, &Size, Data);
  if (!EFI_ERROR (Status) && (Size != Info->FileSize)) {
    Status = EFI_DEVICE_ERROR;
  }

  if (!EFI_ERROR (Status)) {
    Status = FirmwareUpdateVerifyChecksum (Image, ImageSize, Data, Size, ImageName);
  }

Out:
  if (File) {
    File->Close (File);
  }

  if (Info) {
    FreePool (Info);
  }

  if (Data) {
    FreePool (Data);
  }

  FreePool (Path);
  return Status;
}

// ChooseFile can return the last directory when the browser is cancelled.
STATIC BOOLEAN EFIAPI
FileSelected (
  EFI_DEVICE_PATH_PROTOCOL  *Path
  )
{
  mFileSelected = Path != NULL;
  return TRUE;
}

STATIC EFI_STATUS
SelectAndUpdate (
  VOID
  )
{
  EFI_DEVICE_PATH_PROTOCOL       *Path = NULL, *Cursor;
  EFI_FILE_PROTOCOL              *File = NULL;
  EFI_FILE_INFO                  *Info = NULL;
  PLATFORM_FIRMWARE_UPDATE_INFO  UpdateInfo;
  CONST CHAR16                   *Hint  = L"If writing had started, retry before restarting.";
  UINT8                          *Image = NULL;
  UINTN                          Size;
  EFI_STATUS                     Status;
  EFI_INPUT_KEY                  Key;
  CHAR16                         Message[128], Name[96];
  BOOLEAN                        Restart = FALSE;

  PlatformFirmwareUpdateGetInfo (&UpdateInfo);

  // Connect media inserted after Setup opened before listing filesystems.
  EfiBootManagerConnectAll ();
  mFileSelected = FALSE;
  Status        = ChooseFile (NULL, L".bin", FileSelected, &Path);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (!mFileSelected || !Path) {
    Status = EFI_ABORTED;
    goto Done;
  }

  Cursor = Path;
  Status = EfiOpenFileByDevicePath (&Cursor, &File, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Info = FileHandleGetInfo (File);
  if (!Info) {
    Status = EFI_DEVICE_ERROR;
    goto Done;
  }

  if ((Info->Attribute & EFI_FILE_DIRECTORY) || (Info->FileSize != UpdateInfo.ImageSize)) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  UnicodeSPrint (Name, sizeof (Name), L"File: %s", Info->FileName);
  Size  = UpdateInfo.ImageSize;
  Image = AllocatePool (Size);
  if (!Image) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  Status = File->Read (File, &Size, Image);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  File->Close (File);
  File = NULL;
  Status = CheckFileChecksum (Path, Info->FileName, Image, Size);
  if (EFI_ERROR (Status)) {
    Hint = L"Keep the matching SHA256SUMS beside the firmware image.";
    goto Done;
  }

  Status = PlatformFirmwareUpdateValidate (Image, Size);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  do {
    CreatePopUp (
      EFI_LIGHTGRAY | EFI_BACKGROUND_BLUE,
      &Key,
      L"Firmware Update",
      Name,
      UpdateInfo.PreservationNotice,
      L"Do not turn off power during the update.",
      L"Press Y to update, Esc to cancel.",
      NULL
      );
  } while (Key.UnicodeChar != L'y' && Key.UnicodeChar != L'Y' && Key.ScanCode != SCAN_ESC);

  if (Key.ScanCode == SCAN_ESC) {
    Status = EFI_ABORTED;
    goto Done;
  }

  Status = gBS->SetWatchdogTimer (0, 0, 0, NULL);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"Firmware Update\r\n%s\r\nDo not turn off power.\r\n\r\n", Name);
  UPDATE_PROGRESS  State = { FirmwareUpdateRead, MAX_UINTN, (UINTN)gST->ConOut->Mode->CursorRow };

  Status = PlatformFirmwareUpdate (Image, Size, Progress, &State);
  if (!EFI_ERROR (Status)) {
    CreatePopUp (
      EFI_LIGHTGRAY | EFI_BACKGROUND_BLUE,
      &Key,
      L"Firmware written and verified.",
      UpdateInfo.PreservationNotice,
      L"Press Enter to restart, or Esc to return to Setup.",
      NULL
      );
    while (Key.UnicodeChar != CHAR_CARRIAGE_RETURN && Key.ScanCode != SCAN_ESC) {
      UINTN  Index;
      gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
      gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    }

    Restart = Key.UnicodeChar == CHAR_CARRIAGE_RETURN;
  }

Done:
  if (File) {
    File->Close (File);
  }

  if (Info) {
    FreePool (Info);
  }

  if (Image) {
    FreePool (Image);
  }

  if (Path) {
    FreePool (Path);
  }

  if (EFI_ERROR (Status) && (Status != EFI_ABORTED)) {
    UnicodeSPrint (Message, sizeof (Message), L"Firmware update failed: %r", Status);
    CreatePopUp (
      EFI_LIGHTGRAY | EFI_BACKGROUND_BLUE,
      &Key,
      Message,
      Hint,
      L"Press any key to return.",
      NULL
      );
  }

  if (Restart) {
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
  }

  return Status;
}

STATIC EFI_STATUS EFIAPI
ExtractConfig (
  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  CONST EFI_STRING                      Request,
  EFI_STRING                            *Progress,
  EFI_STRING                            *Results
  )
{
  if (!Progress || !Results) {
    return EFI_INVALID_PARAMETER;
  }

  *Progress = Request;
  *Results  = NULL;
  return EFI_NOT_FOUND;
}

STATIC EFI_STATUS EFIAPI
RouteConfig (
  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  CONST EFI_STRING                      Configuration,
  EFI_STRING                            *Progress
  )
{
  if (!Configuration || !Progress) {
    return EFI_INVALID_PARAMETER;
  }

  *Progress = Configuration;
  return EFI_NOT_FOUND;
}

STATIC EFI_STATUS EFIAPI
Callback (
  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  EFI_BROWSER_ACTION                    Action,
  EFI_QUESTION_ID                       QuestionId,
  UINT8                                 Type,
  EFI_IFR_TYPE_VALUE                    *Value,
  EFI_BROWSER_ACTION_REQUEST            *ActionRequest
  )
{
  if ((Action != EFI_BROWSER_ACTION_CHANGING) || (QuestionId != UPDATE_FILE)) {
    return EFI_UNSUPPORTED;
  }

  if (!ActionRequest) {
    return EFI_INVALID_PARAMETER;
  }

  *ActionRequest = EFI_BROWSER_ACTION_REQUEST_NONE;
  SelectAndUpdate ();
  // The updater displayed the result; return normally to the form browser.
  return EFI_SUCCESS;
}

STATIC EFI_HII_CONFIG_ACCESS_PROTOCOL  mAccess = { ExtractConfig, RouteConfig, Callback };

EFI_STATUS EFIAPI
FirmwareUpdateUiConstructor (
  EFI_HANDLE        ImageHandle,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status = gBS->InstallMultipleProtocolInterfaces (
                              &mHandle,
                              &gEfiDevicePathProtocolGuid,
                              &mPath,
                              &gEfiHiiConfigAccessProtocolGuid,
                              &mAccess,
                              NULL
                              );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  mHiiHandle = HiiAddPackages (&mFormGuid, mHandle, FirmwareUpdateUiLibStrings, FormsBin, NULL);
  if (!mHiiHandle) {
    gBS->UninstallMultipleProtocolInterfaces (
           mHandle,
           &gEfiDevicePathProtocolGuid,
           &mPath,
           &gEfiHiiConfigAccessProtocolGuid,
           &mAccess,
           NULL
           );
    mHandle = NULL;
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
FirmwareUpdateUiDestructor (
  EFI_HANDLE        ImageHandle,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  if (mHiiHandle) {
    HiiRemovePackages (mHiiHandle);
    mHiiHandle = NULL;
  }

  if (mHandle) {
    gBS->UninstallMultipleProtocolInterfaces (
           mHandle,
           &gEfiDevicePathProtocolGuid,
           &mPath,
           &gEfiHiiConfigAccessProtocolGuid,
           &mAccess,
           NULL
           );
    mHandle = NULL;
  }

  return EFI_SUCCESS;
}
