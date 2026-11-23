/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Library/Eic7700LocalMacLib.h>

BOOLEAN Eic7700MacPairValid (CONST EIC7700_MAC_PAIR *Pair);
EFI_STATUS Eic7700ReadEepromMacPair (EIC7700_MAC_PAIR *Pair);
