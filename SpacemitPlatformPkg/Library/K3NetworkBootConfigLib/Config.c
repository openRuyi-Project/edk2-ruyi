// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/K3SpiConfigLib.h>
#include <Library/NetworkBootConfigLib.h>

EFI_STATUS
NetworkBootConfigGet (NETWORK_BOOT_CONFIG *Config)
{
  UINT32 Value = 0;
  EFI_STATUS Status;
  if (Config == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = K3SpiConfigGet (K3SettingNetworkBoot, &Value);
  Config->Pxe = (Value & BIT0) != 0;
  Config->Http = (Value & BIT1) != 0;
  return Status;
}

EFI_STATUS
NetworkBootConfigSet (CONST NETWORK_BOOT_CONFIG *Config)
{
  if (Config == NULL || Config->Pxe > 1 || Config->Http > 1) {
    return EFI_INVALID_PARAMETER;
  }

  return K3SpiConfigSet (K3SettingNetworkBoot, (Config->Pxe ? BIT0 : 0) | (Config->Http ? BIT1 : 0));
}
