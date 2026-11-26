/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Library/Eic7700LocalMacLib.h>

#define D560_ETHERNET_PORTS  4

EFI_STATUS D560ReadEnvironmentMacs (EIC7700_MAC Macs[D560_ETHERNET_PORTS]);

BOOLEAN Eic7700MacPairValid (CONST EIC7700_MAC_PAIR *Pair);
EFI_STATUS Eic7700ReadEepromMacPair (EIC7700_MAC_PAIR *Pair);
