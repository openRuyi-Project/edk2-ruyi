/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/MdeModuleHii.h>
#include <Library/CpuConfigLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/HiiConfigAccess.h>
#include <Protocol/HiiConfigRouting.h>
#include <Library/BaseLib.h>
#include <Library/HiiLib.h>
#include <Library/PcdLib.h>
#include <Protocol/PlatformSettings.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryConfigLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "Forms.h"
#include "PlatformDefaultsUiDxeStrDefs.h"

extern UINT8                            SettingsFormsBin[];
extern UINT8                            PlatformDefaultsUiDxeStrings[];
STATIC RUYI_PLATFORM_SETTINGS_PROTOCOL      *mSettings;
STATIC EFI_GUID                         mFormGuid = PLATFORM_FORMSET_GUID;
STATIC EFI_HANDLE                       mHandle;
STATIC EFI_HII_HANDLE                   mHiiHandle;
STATIC EFI_HII_CONFIG_ROUTING_PROTOCOL  *mRouting;
STATIC struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} mPath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,                   { sizeof (VENDOR_DEVICE_PATH),       0 }
    }, PLATFORM_FORMSET_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC EFI_STATUS
ReadSettings (
  PLATFORM_CONFIGURATION  *Config
  )
{
  BOOLEAN Enabled, Present = FALSE;
  UINT32 Defaults, Effective;
  EFI_STATUS Status = mSettings->GetDefaults (&Defaults);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Config->Available        = FixedPcdGetBool (PcdDriverEmulationAvailable);
  Config->EmulationEnabled = Config->Available && !!(Defaults & RUYI_SETTING_EMULATION);
  Config->PxeBoot = !!(Defaults & RUYI_SETTING_PXE);
  Config->HttpBoot = !!(Defaults & RUYI_SETTING_HTTP);
  mSettings->GetEffective (&Effective, &Present);
  Config->OverridesPresent = Present;
  CPU_CONFIG Cpu;
  Config->CpuAvailable = 0;
  Config->CpuMode = 0;
  Status = CpuConfigGet (&Cpu);
  if (!EFI_ERROR (Status)) {
    Config->CpuAvailable = 1;
    Config->CpuMode = Cpu.Default;
  } else if (Status != EFI_UNSUPPORTED) { return Status; }
  Config->EccAvailable = 0;
  Config->EccEnabled = 0;
  Status = MemoryConfigGet (&Enabled);
  if (Status == EFI_UNSUPPORTED) { return EFI_SUCCESS; }
  if (!EFI_ERROR (Status)) {
    Config->EccAvailable = 1;
    Config->EccEnabled = Enabled;
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
  PLATFORM_CONFIGURATION  Config;
  EFI_STATUS               Status;
  EFI_STRING               FullRequest = NULL;
  EFI_STRING               Header;
  UINTN                    Size;

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
  PLATFORM_CONFIGURATION  Config;
  EFI_STATUS               Status;
  UINTN                    Size = sizeof (Config);
  UINT8                    EccAvailable, CpuAvailable;

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

  EccAvailable = Config.EccAvailable;
  CpuAvailable = Config.CpuAvailable;
  Status = mRouting->ConfigToBlock (mRouting, Configuration, (UINT8 *)&Config, &Size, Progress);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Config.EmulationEnabled > 1) ||
      (Config.PxeBoot > 1) || (Config.HttpBoot > 1) ||
      (Config.EccAvailable != EccAvailable) || (Config.EccEnabled > 1) ||
      (!EccAvailable && Config.EccEnabled) ||
      Config.CpuAvailable != CpuAvailable || (!CpuAvailable && Config.CpuMode) ||
      (!Config.Available && Config.EmulationEnabled) ||
      (Config.Available != FixedPcdGetBool (PcdDriverEmulationAvailable)))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (CpuAvailable) {
    CPU_CONFIG Cpu;
    BOOLEAN Valid = FALSE;
    Status = CpuConfigGet (&Cpu);
    if (EFI_ERROR (Status)) { return Status; }
    if (Cpu.Modes == NULL || Cpu.Count == 0 || Cpu.Count > 256) { return EFI_COMPROMISED_DATA; }
    for (UINTN Index = 0; Index < Cpu.Count; Index++) {
      if (Cpu.Modes[Index].Value == Config.CpuMode) { Valid = TRUE; }
    }
    if (!Valid) { return EFI_INVALID_PARAMETER; }
  }

  // Save persistent defaults without depending on UEFI variable writes.
  UINT32 Defaults;
  Status = mSettings->GetDefaults (&Defaults);
  if (EFI_ERROR (Status)) { return Status; }
  Defaults &= ~(RUYI_SETTING_PXE | RUYI_SETTING_HTTP);
  if (Config.PxeBoot) { Defaults |= RUYI_SETTING_PXE; }
  if (Config.HttpBoot) { Defaults |= RUYI_SETTING_HTTP; }
  if (Config.Available) {
    Defaults &= ~RUYI_SETTING_EMULATION;
    if (Config.EmulationEnabled) { Defaults |= RUYI_SETTING_EMULATION; }
  }
  Status = mSettings->SetDefaults (Defaults);
  if (EFI_ERROR (Status)) { return Status; }
  if (EccAvailable) {
    Status = MemoryConfigSet (Config.EccEnabled);
    if (EFI_ERROR (Status)) { return Status; }
  }
  return CpuAvailable ? CpuConfigSet (Config.CpuMode) : EFI_SUCCESS;
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
  if ((Action == EFI_BROWSER_ACTION_DEFAULT_STANDARD || Action == EFI_BROWSER_ACTION_DEFAULT_MANUFACTURING) &&
      QuestionId >= 0x1001 && QuestionId <= 0x1004 && QuestionId != CPU_QUESTION_ID && Value) {
    UINT32 Defaults;
    EFI_STATUS Status = mSettings->GetDefaults (&Defaults);
    if (EFI_ERROR (Status)) { return Status; }
    if (QuestionId == 0x1001) { Value->u8 = FixedPcdGetBool (PcdDriverEmulationAvailable) && !!(Defaults & RUYI_SETTING_EMULATION); }
    else if (QuestionId == 0x1003) { Value->u8 = !!(Defaults & RUYI_SETTING_PXE); }
    else { Value->u8 = !!(Defaults & RUYI_SETTING_HTTP); }
    if (ActionRequest) { *ActionRequest = EFI_BROWSER_ACTION_REQUEST_NONE; }
    return EFI_SUCCESS;
  }
  if (Action == EFI_BROWSER_ACTION_CHANGED && QuestionId == 0x1005) {
    EFI_STATUS Status = mSettings->ClearOverrides ();
    PLATFORM_CONFIGURATION Config;
    if (EFI_ERROR (Status)) { return Status; }
    if (HiiGetBrowserData (&mFormGuid, L"PlatformConfig", sizeof (Config), (UINT8 *)&Config)) {
      Config.OverridesPresent = 0;
      HiiSetBrowserData (&mFormGuid, L"PlatformConfig", sizeof (Config), (UINT8 *)&Config, NULL);
    }
    if (ActionRequest) { *ActionRequest = EFI_BROWSER_ACTION_REQUEST_NONE; }
    return EFI_SUCCESS;
  }
  if ((Action == EFI_BROWSER_ACTION_DEFAULT_STANDARD ||
       Action == EFI_BROWSER_ACTION_DEFAULT_MANUFACTURING) &&
      QuestionId == CPU_QUESTION_ID && Value) {
    CPU_CONFIG Cpu;
    EFI_STATUS Status = CpuConfigGet (&Cpu);
    if (EFI_ERROR (Status)) { return Status; }
    Value->u8 = Cpu.Default;
    if (ActionRequest != NULL) { *ActionRequest = EFI_BROWSER_ACTION_REQUEST_NONE; }
    return EFI_SUCCESS;
  }
  if ((Action == EFI_BROWSER_ACTION_DEFAULT_STANDARD || Action == EFI_BROWSER_ACTION_DEFAULT_MANUFACTURING) &&
      QuestionId == 0x1006 && Value) {
    BOOLEAN Enabled;
    EFI_STATUS Status = MemoryConfigGet (&Enabled);
    if (EFI_ERROR (Status)) { return Status; }
    Value->u8 = Enabled;
    if (ActionRequest) { *ActionRequest = EFI_BROWSER_ACTION_REQUEST_NONE; }
    return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}

STATIC EFI_HII_CONFIG_ACCESS_PROTOCOL  mAccess = { ExtractConfig, RouteConfig, Callback };

STATIC EFI_STATUS
AddCpuSettings (VOID)
{
  CPU_CONFIG Cpu;
  EFI_STATUS Status = CpuConfigGet (&Cpu);
  VOID *Start = NULL, *End = NULL, *Options = NULL;
  EFI_IFR_GUID_LABEL *Label;
  EFI_STRING_ID Help;
  BOOLEAN HasDefault = FALSE;
  if (Status == EFI_UNSUPPORTED) { return EFI_SUCCESS; }
  if (EFI_ERROR (Status)) { return Status; }
  if (Cpu.Modes == NULL || Cpu.Help == NULL || Cpu.Count == 0 || Cpu.Count > 256) {
    return EFI_COMPROMISED_DATA;
  }
  Start = HiiAllocateOpCodeHandle ();
  End = HiiAllocateOpCodeHandle ();
  Options = HiiAllocateOpCodeHandle ();
  Status = EFI_OUT_OF_RESOURCES;
  if (Start == NULL || End == NULL || Options == NULL) { goto Done; }
  Label = (VOID *)HiiCreateGuidOpCode (Start, &gEfiIfrTianoGuid, NULL, sizeof (*Label));
  if (Label == NULL) { goto Done; }
  Label->ExtendOpCode = EFI_IFR_EXTEND_OP_LABEL;
  Label->Number = CPU_LABEL;
  Label = (VOID *)HiiCreateGuidOpCode (End, &gEfiIfrTianoGuid, NULL, sizeof (*Label));
  if (Label == NULL) { goto Done; }
  Label->ExtendOpCode = EFI_IFR_EXTEND_OP_LABEL;
  Label->Number = CPU_LABEL_END;
  Help = HiiSetString (mHiiHandle, 0, (CHAR16 *)Cpu.Help, NULL);
  if (Help == 0) { goto Done; }
  for (UINTN Index = 0; Index < Cpu.Count; Index++) {
    EFI_STRING_ID Name;
    UINT8 Flags = 0;
    if (Cpu.Modes[Index].Name == NULL) { Status = EFI_COMPROMISED_DATA; goto Done; }
    for (UINTN Previous = 0; Previous < Index; Previous++) {
      if (Cpu.Modes[Previous].Value == Cpu.Modes[Index].Value) { Status = EFI_COMPROMISED_DATA; goto Done; }
    }
    Name = HiiSetString (mHiiHandle, 0, (CHAR16 *)Cpu.Modes[Index].Name, NULL);
    if (Name == 0) { goto Done; }
    if (Cpu.Modes[Index].Value == Cpu.Default) {
      Flags = EFI_IFR_OPTION_DEFAULT | EFI_IFR_OPTION_DEFAULT_MFG;
      HasDefault = TRUE;
    }
    if (HiiCreateOneOfOptionOpCode (Options, Name, Flags, EFI_IFR_TYPE_NUM_SIZE_8,
                                  Cpu.Modes[Index].Value) == NULL) { goto Done; }
  }
  if (!HasDefault) { Status = EFI_COMPROMISED_DATA; goto Done; }
  if (HiiCreateOneOfOpCode (Start, CPU_QUESTION_ID, 1,
                           OFFSET_OF (PLATFORM_CONFIGURATION, CpuMode), STRING_TOKEN (STR_CPU_MODE),
                           Help, EFI_IFR_FLAG_RESET_REQUIRED | EFI_IFR_FLAG_CALLBACK,
                           EFI_IFR_NUMERIC_SIZE_1, Options, NULL) == NULL) { goto Done; }
  Status = HiiUpdateForm (mHiiHandle, &mFormGuid, 1, Start, End);
Done:
  if (Options) { HiiFreeOpCodeHandle (Options); }
  if (End) { HiiFreeOpCodeHandle (End); }
  if (Start) { HiiFreeOpCodeHandle (Start); }
  return Status;
}

EFI_STATUS EFIAPI
PlatformDefaultsUiEntry (
  EFI_HANDLE        ImageHandle,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (&gRuyiPlatformSettingsProtocolGuid, NULL, (VOID **)&mSettings);
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

  mHiiHandle = HiiAddPackages (&mFormGuid, mHandle, PlatformDefaultsUiDxeStrings, SettingsFormsBin, NULL);
  Status = mHiiHandle ? AddCpuSettings () : EFI_OUT_OF_RESOURCES;
  if (EFI_ERROR (Status)) {
    if (mHiiHandle) { HiiRemovePackages (mHiiHandle); mHiiHandle = NULL; }
    gBS->UninstallMultipleProtocolInterfaces (
           mHandle,
           &gEfiDevicePathProtocolGuid,
           &mPath,
           &gEfiHiiConfigAccessProtocolGuid,
           &mAccess,
           NULL
           );
    mHandle = NULL;
    return Status;
  }

  return EFI_SUCCESS;
}
