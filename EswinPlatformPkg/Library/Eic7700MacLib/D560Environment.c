// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/Eic7700SpiNorLib.h>
#include <Library/MemoryAllocationLib.h>
#include "Internal.h"

// The non-redundant D560 U-Boot environment is stored on die 0.
#define ENV_OFFSET  0xf00000
#define ENV_SIZE    SIZE_512KB

STATIC VOID
ParseEnvironment (CONST CHAR8 *Data, UINTN Bytes, EIC7700_MAC *Macs)
{
  STATIC CONST CHAR8 *CONST Keys[] = { "ethaddr=", "eth1addr=", "eth2addr=", "eth3addr=" };
  CONST CHAR8 *End = Data + Bytes;

  while (Data < End && *Data != '\0') {
    CONST CHAR8 *Next = ScanMem8 (Data, End - Data, 0);
    if (Next == NULL) { return; }
    for (UINTN Port = 0; Port < ARRAY_SIZE (Keys); Port++) {
      UINTN Length = AsciiStrLen (Keys[Port]);
      if ((UINTN)(Next - Data) != Length + 3 * PXE_HWADDR_LEN_ETHER - 1 ||
          CompareMem (Data, Keys[Port], Length) != 0) {
        continue;
      }

      EIC7700_MAC Octets;
      CONST CHAR8 *Value = Data + Length;
      UINTN Byte;
      for (Byte = 0; Byte < sizeof (Octets); Byte++) {
        if ((Byte != 0 && Value[3 * Byte - 1] != ':') ||
            RETURN_ERROR (AsciiStrHexToBytes (Value + 3 * Byte, 2, &Octets[Byte], 1))) {
          break;
        }
      }
      if (Byte == sizeof (Octets)) { CopyMem (Macs[Port], Octets, sizeof (Octets)); }
    }
    Data = Next + 1;
  }
}

EFI_STATUS
D560ReadEnvironmentMacs (EIC7700_MAC Macs[D560_ETHERNET_PORTS])
{
  UINT8 *Image = AllocatePool (ENV_SIZE);
  EFI_STATUS Status;
  if (Image == NULL) { return EFI_OUT_OF_RESOURCES; }
  Status = Eic7700SpiNorRead (ENV_OFFSET, ENV_SIZE, Image);
  if (!EFI_ERROR (Status) && ReadUnaligned32 ((CONST UINT32 *)Image) ==
      CalculateCrc32 (Image + sizeof (UINT32), ENV_SIZE - sizeof (UINT32))) {
    ParseEnvironment ((CONST CHAR8 *)Image + sizeof (UINT32), ENV_SIZE - sizeof (UINT32), Macs);
  }
  FreePool (Image);
  return Status;
}
