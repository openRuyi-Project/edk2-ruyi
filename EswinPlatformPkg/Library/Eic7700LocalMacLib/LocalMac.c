// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseCryptLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/Eic7700LocalMacLib.h>
#include <Library/Eic7700SpiNorLib.h>

EFI_STATUS
Eic7700GenerateMacPair (UINT8 Mac[2][6])
{
  UINT8 Seed[EIC7700_SPI_NOR_MAX_UID_SIZE + 1], Hash[SHA256_DIGEST_SIZE];
  UINTN Size = sizeof (Seed) - 1;
  EFI_STATUS Status;

  Status = Eic7700SpiNorInit (FALSE);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Eic7700SpiNorReadUniqueId (Seed, &Size);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN Port = 0; Port < 2; Port++) {
    Seed[Size] = (UINT8)Port;
    if (!Sha256HashAll (Seed, Size + 1, Hash)) { return EFI_DEVICE_ERROR; }
    Mac[Port][0] = 0x02;
    CopyMem (Mac[Port] + 1, Hash, 5);
  }
  if (CompareMem (Mac[0], Mac[1], 6) == 0) {
    Mac[1][5] ^= 1;
  }
  return EFI_SUCCESS;
}
