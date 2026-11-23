// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Uefi/UefiPxe.h>

typedef struct {
  UINT8 Address[2][PXE_HWADDR_LEN_ETHER];
} EIC7700_MAC_PAIR;

// Derive two distinct locally administered addresses from the boot NOR UID.
EFI_STATUS Eic7700GenerateMacPair (EIC7700_MAC_PAIR *Pair);
