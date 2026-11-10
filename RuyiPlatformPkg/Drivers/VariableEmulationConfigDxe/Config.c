/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/EmulationConfig.h>

STATIC EFI_STATUS EFIAPI
Get (BOOLEAN *Enabled, BOOLEAN *DefaultEnabled)
{
  EFI_STATUS Status;
  UINTN Size = sizeof (UINT8);
  UINT8 Saved;

  if (Enabled == NULL || DefaultEnabled == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *DefaultEnabled = FixedPcdGetBool (PcdDriverEmulationAvailable) &&
                    FixedPcdGetBool (PcdDefaultDriverEmulation);
  *Enabled = *DefaultEnabled;
  Status = gRT->GetVariable (L"DriverEmulation", &gRuyiEmulationConfigProtocolGuid,
                            NULL, &Size, &Saved);
  if (Status == EFI_NOT_FOUND || Status == EFI_BUFFER_TOO_SMALL) {
    return EFI_SUCCESS;
  }

  if (!EFI_ERROR (Status) && Size == sizeof (Saved) && Saved <= 1) {
    *Enabled = FixedPcdGetBool (PcdDriverEmulationAvailable) && Saved != 0;
  }

  return Status;
}

STATIC EFI_STATUS EFIAPI
Set (BOOLEAN Enabled)
{
  EFI_STATUS Status;
  BOOLEAN Current, DefaultEnabled;
  UINT8 Saved = Enabled;

  if (Enabled > 1) {
    return EFI_INVALID_PARAMETER;
  }

  if (Enabled && !FixedPcdGetBool (PcdDriverEmulationAvailable)) {
    return EFI_UNSUPPORTED;
  }

  Status = Get (&Current, &DefaultEnabled);
  if (EFI_ERROR (Status) || Current == Enabled) {
    return Status;
  }

  // Apply the saved policy during the next boot, before the emulator runs.
  return gRT->SetVariable (L"DriverEmulation", &gRuyiEmulationConfigProtocolGuid,
                          EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                          EFI_VARIABLE_RUNTIME_ACCESS, sizeof (Saved), &Saved);
}

EFI_STATUS EFIAPI
VariableEmulationConfigEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  STATIC RUYI_EMULATION_CONFIG_PROTOCOL Protocol = { Get, Set };
  EFI_STATUS Status;
  BOOLEAN Enabled, DefaultEnabled;

  Status = Get (&Enabled, &DefaultEnabled);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Driver emulation settings: %r\n", Status));
    return Status;
  }

  Status = PcdSetBoolS (PcdMauEmulationEnabled, Enabled);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((DEBUG_INFO, "UEFI driver emulation: %a\n", Enabled ? "enabled" : "disabled"));
  return gBS->InstallProtocolInterface (&Image, &gRuyiEmulationConfigProtocolGuid,
                                        EFI_NATIVE_INTERFACE, &Protocol);
}
