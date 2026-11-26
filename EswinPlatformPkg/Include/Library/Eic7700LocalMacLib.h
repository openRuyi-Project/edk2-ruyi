// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Uefi/UefiPxe.h>

typedef UINT8 EIC7700_MAC[PXE_HWADDR_LEN_ETHER];

typedef struct {
  EIC7700_MAC Address[2];
} EIC7700_MAC_PAIR;

// Fill empty slots from the boot NOR UID without replacing occupied addresses.
EFI_STATUS Eic7700FillLocalMacs (EIC7700_MAC *Macs, UINTN Count);
BOOLEAN Eic7700MacInUse (CONST EIC7700_MAC *Macs, UINTN Count, CONST UINT8 *Address);

// Derive two distinct locally administered addresses from the boot NOR UID.
EFI_STATUS Eic7700GenerateMacPair (EIC7700_MAC_PAIR *Pair);
