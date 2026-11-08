// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BootManagerLib.h>
#include <Library/DebugLib.h>
#include <Library/NetworkBootConfigLib.h>
#include <Library/PcdLib.h>

VOID EFIAPI
BootManagerConfigureNetworkBoot (VOID)
{
  NETWORK_BOOT_CONFIG Config = { FALSE, FALSE };
  EFI_STATUS Status = NetworkBootConfigGet (&Config);
  if (EFI_ERROR (Status)) {
    Config.Pxe = Config.Http = FALSE;
    DEBUG ((DEBUG_WARN, "Network boot settings: %r\n", Status));
  }

  PcdSet8S (PcdIPv4PXESupport, Config.Pxe);
  PcdSet8S (PcdIPv6PXESupport, Config.Pxe);
  PcdSetBoolS (PcdIPv4HttpSupport, Config.Http);
  PcdSetBoolS (PcdIPv6HttpSupport, Config.Http);
  DEBUG ((DEBUG_INFO, "Network boot: PXE %a, HTTP %a\n",
          Config.Pxe ? "enabled" : "disabled", Config.Http ? "enabled" : "disabled"));
}
