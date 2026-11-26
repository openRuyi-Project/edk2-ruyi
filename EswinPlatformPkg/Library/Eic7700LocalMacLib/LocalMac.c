// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseCryptLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/Eic7700LocalMacLib.h>
#include <Library/Eic7700SpiNorLib.h>

BOOLEAN
Eic7700MacInUse (CONST EIC7700_MAC *Macs, UINTN Count, CONST UINT8 *Address)
{
  for (UINTN Port = 0; Port < Count; Port++) {
    if (CompareMem (Macs[Port], Address, sizeof (EIC7700_MAC)) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

EFI_STATUS
Eic7700FillLocalMacs (EIC7700_MAC *Macs, UINTN Count)
{
  UINT8 Seed[EIC7700_SPI_NOR_MAX_UID_SIZE + 1], Hash[SHA256_DIGEST_SIZE];
  UINTN Size = sizeof (Seed) - 1;
  EIC7700_MAC Candidate;
  EFI_STATUS Status;

  Status = Eic7700SpiNorInit (FALSE);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Eic7700SpiNorReadUniqueId (Seed, &Size);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN Port = 0; Port < Count; Port++) {
    if (!IsZeroBuffer (Macs[Port], sizeof (EIC7700_MAC))) { continue; }
    Seed[Size] = (UINT8)Port;
    if (!Sha256HashAll (Seed, Size + 1, Hash)) { return EFI_DEVICE_ERROR; }
    Candidate[0] = 0x02;
    CopyMem (Candidate + 1, Hash, sizeof (Candidate) - 1);
    // The first collision uses the same bit flip as the two-port implementation.
    for (UINTN Retry = 0; Eic7700MacInUse (Macs, Count, Candidate); Retry++) {
      Candidate[sizeof (Candidate) - 1] = Hash[sizeof (Candidate) - 2] ^ (UINT8)(Retry + 1);
    }
    CopyMem (Macs[Port], Candidate, sizeof (Candidate));
  }
  return EFI_SUCCESS;
}

EFI_STATUS
Eic7700GenerateMacPair (EIC7700_MAC_PAIR *Pair)
{
  ZeroMem (Pair, sizeof (*Pair));
  return Eic7700FillLocalMacs (Pair->Address, ARRAY_SIZE (Pair->Address));
}
