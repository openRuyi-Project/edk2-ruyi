/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/K3SpiConfigLib.h>
#include <Library/PcdLib.h>

EFI_STATUS EFIAPI
K3EmulationPolicyConstructor (
  EFI_HANDLE        ImageHandle,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32      Enabled = 0;
  EFI_STATUS  Status;

  // NvStorage runs after SPI discovery and before variable architecture is
  // published. The emulator already waits for that architecture protocol.
  Status = K3SpiConfigGet (K3SettingDriverEmulation, &Enabled);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "K3 emulation policy read failed: %r; disabled\n", Status));
    Enabled = 0;
  }

  Status = PcdSetBoolS (PcdMauEmulationEnabled, Enabled != 0);
  DEBUG ((DEBUG_INFO, "K3 driver emulation: %a (%r)\n", Enabled ? "enabled" : "disabled", Status));
  return Status;
}
