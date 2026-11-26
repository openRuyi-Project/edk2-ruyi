/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/PcdLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/PlatformSettings.h>
#include "Defaults.h"

#define DEFAULTS_ATTRIBUTES  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS)

EFI_STATUS EFIAPI
GetDefaults (UINT32 *Values)
{
  UINT32 Attributes = 0;
  UINT32 Initial = FixedPcdGetBool (PcdDriverEmulationAvailable) ? RUYI_SETTING_EMULATION : 0;
  UINTN Size = sizeof (*Values);
  EFI_STATUS Status;
  if (!Values) { return EFI_INVALID_PARAMETER; }
  Status = gRT->GetVariable (L"PlatformDefaults", &gRuyiPlatformSettingsProtocolGuid,
                             &Attributes, &Size, Values);
  if (Status == EFI_NOT_FOUND) {
    *Values = Initial;
    return EFI_SUCCESS;
  }
  if (!EFI_ERROR (Status)) {
    if (Size == sizeof (*Values) && !(*Values & ~RUYI_SETTINGS_MASK) &&
        (!FixedPcdGetBool (PcdRepairInvalidPlatformDefaults) || Attributes == DEFAULTS_ATTRIBUTES)) {
      return EFI_SUCCESS;
    }
    Status = EFI_COMPROMISED_DATA;
  }
  if (!FixedPcdGetBool (PcdRepairInvalidPlatformDefaults) ||
      (Status != EFI_COMPROMISED_DATA && Status != EFI_BUFFER_TOO_SMALL)) {
    return Status;
  }
  // Recreate only the invalid setting. Other variables share this store.
  Status = gRT->SetVariable (L"PlatformDefaults", &gRuyiPlatformSettingsProtocolGuid, 0, 0, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  *Values = Initial;
  return SetDefaults (Initial);
}

EFI_STATUS EFIAPI
SetDefaults (UINT32 Values)
{
  if (Values & ~RUYI_SETTINGS_MASK) { return EFI_INVALID_PARAMETER; }
  return gRT->SetVariable (L"PlatformDefaults", &gRuyiPlatformSettingsProtocolGuid,
           DEFAULTS_ATTRIBUTES, sizeof (Values), &Values);
}
