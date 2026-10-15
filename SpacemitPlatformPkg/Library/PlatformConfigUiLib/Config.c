/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Protocol/DevicePath.h>
#include <Protocol/HiiConfigAccess.h>
#include <Protocol/HiiConfigRouting.h>
#include <Library/BaseLib.h>
#include <Library/HiiLib.h>
#include <Library/K3CpuConfigLib.h>
#include <Library/K3SpiConfigLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "Forms.h"

extern UINT8                            PlatformFormsBin[];
extern UINT8                            PlatformConfigUiLibStrings[];
STATIC EFI_GUID                         mFormGuid = K3_PLATFORM_FORMSET_GUID;
STATIC EFI_HANDLE                       mHandle;
STATIC EFI_HII_HANDLE                   mHiiHandle;
STATIC EFI_HII_CONFIG_ROUTING_PROTOCOL  *mRouting;
STATIC struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} mPath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,                   { sizeof (VENDOR_DEVICE_PATH),       0 }
    }, K3_PLATFORM_FORMSET_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC EFI_STATUS
ReadSettings (
  K3_PLATFORM_CONFIGURATION  *Config
  )
{
  UINT32      Value;
  EFI_STATUS  Status;

  Config->CpuMode          = 0;
  Config->EmulationEnabled = 0;
  Status                   = K3SpiConfigGet (K3SettingCpuMode, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Config->CpuMode = (UINT8)Value;
 #ifdef K3_X64EMU
  Status = K3SpiConfigGet (K3SettingDriverEmulation, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Config->EmulationEnabled = (UINT8)Value;
 #endif
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
ExtractConfig (
  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  CONST EFI_STRING                      Request,
  EFI_STRING                            *Progress,
  EFI_STRING                            *Results
  )
{
  K3_PLATFORM_CONFIGURATION  Config;
  EFI_STATUS                 Status;
  EFI_STRING                 FullRequest = NULL;
  EFI_STRING                 Header;
  UINTN                      Size;

  if (!Progress || !Results) {
    return EFI_INVALID_PARAMETER;
  }

  *Progress = Request;
  *Results  = NULL;
  if (Request && !HiiIsConfigHdrMatch (Request, &mFormGuid, L"PlatformConfig")) {
    return EFI_NOT_FOUND;
  }

  Status = ReadSettings (&Config);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Request || !StrStr (Request, L"&OFFSET=")) {
    Header = HiiConstructConfigHdr (&mFormGuid, L"PlatformConfig", mHandle);
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
  K3_PLATFORM_CONFIGURATION  Config;
  EFI_STATUS                 Status;
  UINTN                      Size = sizeof (Config);

  if (!Configuration || !Progress) {
    return EFI_INVALID_PARAMETER;
  }

  *Progress = Configuration;
  if (!HiiIsConfigHdrMatch (Configuration, &mFormGuid, L"PlatformConfig")) {
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

  // Validate both fields before changing either independent SPI record.
  if ((Config.CpuMode > K3CpuA100Only) || (Config.EmulationEnabled > 1)) {
    return EFI_INVALID_PARAMETER;
  }

 #ifdef K3_X64EMU
  Status = K3SpiConfigSet (K3SettingDriverEmulation, Config.EmulationEnabled);
  if (EFI_ERROR (Status)) {
    return Status;
  }

 #else
  if (Config.EmulationEnabled != 0) {
    return EFI_UNSUPPORTED;
  }

 #endif
  return K3SpiConfigSet (K3SettingCpuMode, Config.CpuMode);
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
  return EFI_UNSUPPORTED;
}

STATIC EFI_HII_CONFIG_ACCESS_PROTOCOL  mAccess = { ExtractConfig, RouteConfig, Callback };

EFI_STATUS EFIAPI
PlatformConfigUiConstructor (
  EFI_HANDLE        ImageHandle,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

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

  mHiiHandle = HiiAddPackages (&mFormGuid, mHandle, PlatformConfigUiLibStrings, PlatformFormsBin, NULL);
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
PlatformConfigUiDestructor (
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
