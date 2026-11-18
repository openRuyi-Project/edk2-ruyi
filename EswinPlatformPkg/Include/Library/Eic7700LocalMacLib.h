// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

// Derive two distinct locally administered addresses from the boot NOR UID.
EFI_STATUS Eic7700GenerateMacPair (UINT8 Mac[2][6]);
