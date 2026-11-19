/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/PcdLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/PlatformSettings.h>
#include "Defaults.h"

EFI_STATUS EFIAPI
GetDefaults (UINT32 *Values)
{
  UINTN Size = sizeof (*Values);
  if (!Values) { return EFI_INVALID_PARAMETER; }
  EFI_STATUS Status = gRT->GetVariable (L"PlatformDefaults", &gRuyiPlatformSettingsProtocolGuid,
                                       NULL, &Size, Values);
  if (Status == EFI_NOT_FOUND) {
    *Values = FixedPcdGetBool (PcdDriverEmulationAvailable) ? RUYI_SETTING_EMULATION : 0;
    return EFI_SUCCESS;
  }
  if (EFI_ERROR (Status)) { return Status; }
  return (Size == sizeof (*Values) && !(*Values & ~RUYI_SETTINGS_MASK)) ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
}

EFI_STATUS EFIAPI
SetDefaults (UINT32 Values)
{
  if (Values & ~RUYI_SETTINGS_MASK) { return EFI_INVALID_PARAMETER; }
  return gRT->SetVariable (L"PlatformDefaults", &gRuyiPlatformSettingsProtocolGuid,
           EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS, sizeof (Values), &Values);
}
