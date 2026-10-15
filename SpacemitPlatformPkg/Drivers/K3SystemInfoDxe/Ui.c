/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Info.h"
#include <Guid/MdeModuleHii.h>
#include <Protocol/HiiConfigAccess.h>
#include <Protocol/DevicePath.h>
#include <Library/HiiLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define INFO_PAGE_ROWS  9
#define MAX_INFO_ROWS   (INFO_PAGE_ROWS + 2)

extern UINT8           SystemInfoBin[];
extern UINT8           K3SystemInfoDxeStrings[];
STATIC EFI_GUID        mFormGuid = K3_INFO_FORMSET_GUID;
STATIC EFI_HANDLE      mHandle;
STATIC EFI_HII_HANDLE  mHiiHandle;
STATIC EFI_STRING_ID   mStrings[4][MAX_INFO_ROWS][2];
STATIC UINTN           mPageIndex[4];
STATIC UINTN           mPageCount[4];
STATIC struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} mPath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,                   { sizeof (VENDOR_DEVICE_PATH),       0 }
    }, K3_INFO_FORMSET_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

typedef struct {
  VOID     *Opcodes;
  UINTN    Page;
  UINTN    Row;
  UINTN    Seen;
  UINTN    Offset;
} PAGE_CONTEXT;

STATIC EFI_STATUS
AddRow (
  VOID          *Context,
  CONST CHAR16  *Label,
  CONST CHAR16  *Value
  )
{
  PAGE_CONTEXT   *Page = Context;
  EFI_STRING_ID  *Ids;

  if (Page->Row >= MAX_INFO_ROWS) {
    return EFI_BUFFER_TOO_SMALL;
  }

  Ids    = mStrings[Page->Page][Page->Row++];
  Ids[0] = HiiSetString (mHiiHandle, Ids[0], (CHAR16 *)Label, NULL);
  Ids[1] = HiiSetString (mHiiHandle, Ids[1], (CHAR16 *)Value, NULL);
  if (!Ids[0] || !Ids[1] || !HiiCreateTextOpCode (Page->Opcodes, Ids[0], 0, Ids[1])) {
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
CollectRow (
  VOID          *Context,
  CONST CHAR16  *Label,
  CONST CHAR16  *Value
  )
{
  PAGE_CONTEXT  *Page = Context;
  UINTN         Index = Page->Seen++;

  if ((Index < Page->Offset) || (Index - Page->Offset >= INFO_PAGE_ROWS)) {
    return EFI_SUCCESS;
  }

  return AddRow (Page, Label, Value);
}

STATIC EFI_STATUS
UpdatePage (
  UINT16  Form
  )
{
  VOID                *Start = HiiAllocateOpCodeHandle (), *End = HiiAllocateOpCodeHandle ();
  EFI_IFR_GUID_LABEL  *Label;
  PAGE_CONTEXT        Page   = { Start, Form - INFO_CPU, 0, 0, 0 };
  EFI_STATUS          Status = EFI_OUT_OF_RESOURCES;
  CHAR16              Text[96];
  BOOLEAN             Retry = FALSE;

  if (!Start || !End) {
    goto Done;
  }

  Page.Offset = mPageIndex[Page.Page] * INFO_PAGE_ROWS;
  Label       = (VOID *)HiiCreateGuidOpCode (Start, &gEfiIfrTianoGuid, NULL, sizeof (*Label));
  if (!Label) {
    goto Done;
  }

  Label->ExtendOpCode = EFI_IFR_EXTEND_OP_LABEL;
  Label->Number       = Form;
  Label               = (VOID *)HiiCreateGuidOpCode (End, &gEfiIfrTianoGuid, NULL, sizeof (*Label));
  if (!Label) {
    goto Done;
  }

  Label->ExtendOpCode = EFI_IFR_EXTEND_OP_LABEL;
  Label->Number       = INFO_END;
  Status              = AddRow (&Page, L"Page", L"");
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status                = CollectSystemInfo (Form, CollectRow, &Page);
  mPageCount[Page.Page] = MAX (1, (Page.Seen + INFO_PAGE_ROWS - 1) / INFO_PAGE_ROWS);
  if (mPageIndex[Page.Page] >= mPageCount[Page.Page]) {
    mPageIndex[Page.Page] = 0;
    Retry                 = TRUE;
    goto Done;
  }

  if (EFI_ERROR (Status)) {
    if ((Status == EFI_NOT_FOUND) && (Page.Seen == 0)) {
      UnicodeSPrint (Text, sizeof (Text), L"No devices detected");
    } else {
      UnicodeSPrint (Text, sizeof (Text), L"Information unavailable: %r", Status);
    }

    Status = AddRow (&Page, L"Status", Text);
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }

  UnicodeSPrint (Text, sizeof (Text), L"%u / %u", mPageIndex[Page.Page] + 1, mPageCount[Page.Page]);
  if (!HiiSetString (mHiiHandle, mStrings[Page.Page][0][1], Text, NULL)) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  Status = HiiUpdateForm (mHiiHandle, &mFormGuid, Form, Start, End);
Done:
  if (Start) {
    HiiFreeOpCodeHandle (Start);
  }

  if (End) {
    HiiFreeOpCodeHandle (End);
  }

  return Retry ? UpdatePage (Form) : Status;
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
  if (!Progress || !Configuration) {
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
  UINT16  Form      = QuestionId & 0xff;
  UINT16  Operation = QuestionId & 0xff00;

  if ((Action != EFI_BROWSER_ACTION_CHANGING) || (Form < INFO_CPU) || (Form > INFO_STORAGE) ||
      (Operation > INFO_NEXT))
  {
    return EFI_UNSUPPORTED;
  }

  if (!ActionRequest) {
    return EFI_INVALID_PARAMETER;
  }

  *ActionRequest = EFI_BROWSER_ACTION_REQUEST_NONE;
  UINTN  Page = Form - INFO_CPU;

  if (Operation == INFO_PREVIOUS) {
    if (mPageIndex[Page]) {
      mPageIndex[Page]--;
    }
  } else if (Operation == INFO_NEXT) {
    if (mPageIndex[Page] + 1 < mPageCount[Page]) {
      mPageIndex[Page]++;
    }
  } else {
    mPageIndex[Page] = 0;
  }

  return UpdatePage (Form);
}

STATIC EFI_HII_CONFIG_ACCESS_PROTOCOL  mAccess = { ExtractConfig, RouteConfig, Callback };

EFI_STATUS EFIAPI
K3SystemInfoEntry (
  EFI_HANDLE        ImageHandle,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->InstallMultipleProtocolInterfaces (
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

  mHiiHandle = HiiAddPackages (&mFormGuid, mHandle, K3SystemInfoDxeStrings, SystemInfoBin, NULL);
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
K3SystemInfoUnload (
  EFI_HANDLE  ImageHandle
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
