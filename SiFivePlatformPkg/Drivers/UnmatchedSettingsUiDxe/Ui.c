/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Protocol/DevicePath.h>
#include <Protocol/HiiConfigAccess.h>
#include <Protocol/HiiConfigRouting.h>
#include <Library/BaseLib.h>
#include <Library/HiiLib.h>
#include <Library/PcdLib.h>
#include <Protocol/UnmatchedSettings.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NetworkBootConfigLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "Forms.h"

extern UINT8                            SettingsFormsBin[];
extern UINT8                            UnmatchedSettingsUiDxeStrings[];
STATIC UNMATCHED_SETTINGS_PROTOCOL      *mSettings;
STATIC EFI_GUID                         mFormGuid = UNMATCHED_FORMSET_GUID;
STATIC EFI_HANDLE                       mHandle;
STATIC EFI_HII_HANDLE                   mHiiHandle;
STATIC EFI_HII_CONFIG_ROUTING_PROTOCOL  *mRouting;
STATIC struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} mPath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,                   { sizeof (VENDOR_DEVICE_PATH),       0 }
    }, UNMATCHED_FORMSET_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC EFI_STATUS
ReadSettings (
  UNMATCHED_CONFIGURATION  *Config
  )
{
  BOOLEAN     Enabled, DefaultEnabled;
  NETWORK_BOOT_CONFIG Network;
  EFI_STATUS  Status = mSettings->Get (&Enabled, &DefaultEnabled);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Config->Available        = FixedPcdGetBool (PcdUnmatchedEmulationAvailable);
  Config->EmulationEnabled = Config->Available && Enabled;
  Status = NetworkBootConfigGet (&Network);
  Config->PxeBoot = Network.Pxe;
  Config->HttpBoot = Network.Http;
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
  UNMATCHED_CONFIGURATION  Config;
  EFI_STATUS               Status;
  EFI_STRING               FullRequest = NULL;
  EFI_STRING               Header;
  UINTN                    Size;

  if (!Progress || !Results) {
    return EFI_INVALID_PARAMETER;
  }

  *Progress = Request;
  *Results  = NULL;
  if (Request && !HiiIsConfigHdrMatch (Request, &mFormGuid, L"UnmatchedConfig")) {
    return EFI_NOT_FOUND;
  }

  Status = ReadSettings (&Config);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Request || !StrStr (Request, L"&OFFSET=")) {
    Header = HiiConstructConfigHdr (&mFormGuid, L"UnmatchedConfig", mHandle);
    if (!Header) {
      return EFI_OUT_OF_RESOURCES;
    }

    Size        = StrSize (Header) + 64 * sizeof (CHAR16);
    FullRequest = AllocatePool (Size);
    if (FullRequest) {
      UnicodeSPrint (FullRequest, Size, L"%s&OFFSET=0&WIDTH=%016LX", Header, (UINT64)sizeof (Config));
    }

    FreePool (Header);
    if (!FullRequest) {
      return EFI_OUT_OF_RESOURCES;
    }
  }

  Status = mRouting->BlockToConfig (
                       mRouting,
                       FullRequest ? FullRequest : Request,
                       (UINT8 *)&Config,
                       sizeof (Config),
                       Results,
                       Progress
                       );
  if (FullRequest) {
    *Progress = Request ? Request + StrLen (Request) : NULL;
    FreePool (FullRequest);
  }

  return Status;
}

STATIC EFI_STATUS EFIAPI
RouteConfig (
  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  CONST EFI_STRING                      Configuration,
  EFI_STRING                            *Progress
  )
{
  UNMATCHED_CONFIGURATION  Config;
  EFI_STATUS               Status;
  UINTN                    Size = sizeof (Config);

  if (!Configuration || !Progress) {
    return EFI_INVALID_PARAMETER;
  }

  *Progress = Configuration;
  if (!HiiIsConfigHdrMatch (Configuration, &mFormGuid, L"UnmatchedConfig")) {
    return EFI_NOT_FOUND;
  }

  Status = ReadSettings (&Config);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = mRouting->ConfigToBlock (mRouting, Configuration, (UINT8 *)&Config, &Size, Progress);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Config.EmulationEnabled > 1) ||
      (Config.PxeBoot > 1) || (Config.HttpBoot > 1) ||
      (Config.Available != FixedPcdGetBool (PcdUnmatchedEmulationAvailable)))
  {
    return EFI_INVALID_PARAMETER;
  }

  Status = mSettings->Set (Config.EmulationEnabled);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  NETWORK_BOOT_CONFIG Network = { Config.PxeBoot, Config.HttpBoot };
  return NetworkBootConfigSet (&Network);
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
  if (((Action == EFI_BROWSER_ACTION_DEFAULT_STANDARD) ||
       (Action == EFI_BROWSER_ACTION_DEFAULT_MANUFACTURING)) && (QuestionId == 0x1001) && Value)
  {
    BOOLEAN     Enabled, DefaultEnabled;
    EFI_STATUS  Status = mSettings->Get (&Enabled, &DefaultEnabled);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Value->u8 = FixedPcdGetBool (PcdUnmatchedEmulationAvailable) && DefaultEnabled;
    if (ActionRequest != NULL) {
      *ActionRequest = EFI_BROWSER_ACTION_REQUEST_NONE;
    }
    return EFI_SUCCESS;
  }

  return EFI_UNSUPPORTED;
}

STATIC EFI_HII_CONFIG_ACCESS_PROTOCOL  mAccess = { ExtractConfig, RouteConfig, Callback };

EFI_STATUS EFIAPI
UnmatchedSettingsUiEntry (
  EFI_HANDLE        ImageHandle,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (&gUnmatchedSettingsProtocolGuid, NULL, (VOID **)&mSettings);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->LocateProtocol (&gEfiHiiConfigRoutingProtocolGuid, NULL, (VOID **)&mRouting);
  if (EFI_ERROR (Status)) {
    return Status;
  }

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

  mHiiHandle = HiiAddPackages (&mFormGuid, mHandle, UnmatchedSettingsUiDxeStrings, SettingsFormsBin, NULL);
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
