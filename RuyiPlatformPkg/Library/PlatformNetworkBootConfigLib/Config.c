/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/NetworkBootConfigLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/PlatformSettings.h>
EFI_STATUS NetworkBootConfigGet (NETWORK_BOOT_CONFIG *Config)
{
  RUYI_PLATFORM_SETTINGS_PROTOCOL *Settings;
  UINT32 Values = 0;
  EFI_STATUS Status;
  if (Config == NULL) { return EFI_INVALID_PARAMETER; }
  Config->Pxe = Config->Http = FALSE;
  Status = gBS->LocateProtocol (&gRuyiPlatformSettingsProtocolGuid, NULL, (VOID **)&Settings);
  if (!EFI_ERROR (Status)) { Status = Settings->GetEffective (&Values, NULL); }
  if (!EFI_ERROR (Status)) { Config->Pxe = !!(Values & RUYI_SETTING_PXE); Config->Http = !!(Values & RUYI_SETTING_HTTP); }
  return Status;
}
EFI_STATUS NetworkBootConfigSet (CONST NETWORK_BOOT_CONFIG *Config)
{
  RUYI_PLATFORM_SETTINGS_PROTOCOL *Settings;
  UINT32 Values;
  EFI_STATUS Status;
  if (Config == NULL || Config->Pxe > 1 || Config->Http > 1) { return EFI_INVALID_PARAMETER; }
  Status = gBS->LocateProtocol (&gRuyiPlatformSettingsProtocolGuid, NULL, (VOID **)&Settings);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Settings->GetDefaults (&Values);
  if (EFI_ERROR (Status)) { return Status; }
  Values = (Values & ~(RUYI_SETTING_PXE | RUYI_SETTING_HTTP)) |
           (Config->Pxe ? RUYI_SETTING_PXE : 0) | (Config->Http ? RUYI_SETTING_HTTP : 0);
  return Settings->SetDefaults (Values);
}
