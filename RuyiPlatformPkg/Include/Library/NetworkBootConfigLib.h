// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

typedef struct {
  BOOLEAN Pxe;
  BOOLEAN Http;
} NETWORK_BOOT_CONFIG;

EFI_STATUS
NetworkBootConfigGet (NETWORK_BOOT_CONFIG *Config);

EFI_STATUS
NetworkBootConfigSet (CONST NETWORK_BOOT_CONFIG *Config);
