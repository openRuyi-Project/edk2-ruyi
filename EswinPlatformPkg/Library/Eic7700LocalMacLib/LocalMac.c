// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseCryptLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/Eic7700LocalMacLib.h>
#include <Library/Eic7700SpiNorLib.h>

EFI_STATUS
Eic7700GenerateMacPair (EIC7700_MAC_PAIR *Pair)
{
  UINT8 Seed[EIC7700_SPI_NOR_MAX_UID_SIZE + 1], Hash[SHA256_DIGEST_SIZE];
  UINTN Size = sizeof (Seed) - 1;
  EFI_STATUS Status;

  Status = Eic7700SpiNorInit (FALSE);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Eic7700SpiNorReadUniqueId (Seed, &Size);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN Port = 0; Port < ARRAY_SIZE (Pair->Address); Port++) {
    Seed[Size] = (UINT8)Port;
    if (!Sha256HashAll (Seed, Size + 1, Hash)) { return EFI_DEVICE_ERROR; }
    Pair->Address[Port][0] = 0x02;
    CopyMem (Pair->Address[Port] + 1, Hash, sizeof (Pair->Address[Port]) - 1);
  }
  if (CompareMem (Pair->Address[0], Pair->Address[1], sizeof (Pair->Address[0])) == 0) {
    Pair->Address[1][sizeof (Pair->Address[1]) - 1] ^= 1;
  }
  return EFI_SUCCESS;
}
