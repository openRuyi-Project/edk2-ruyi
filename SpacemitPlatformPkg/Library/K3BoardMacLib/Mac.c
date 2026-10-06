/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/K3BoardEepromLib.h>
#include <Library/PlatformMacAddressLib.h>
#include <Library/SpacemitTlvLib.h>

EFI_STATUS
PlatformGetMacAddress (
  UINTN  Index,
  UINT8  Mac[6]
  )
{
  if (!Mac) {
    return EFI_INVALID_PARAMETER;
  }

  UINT8       Data[256];
  EFI_STATUS  Status = K3BoardEepromAccess (Data, FALSE, 0);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  UINTN  Offset;

  Status = SpacemitTlvFind (
             Data,
             sizeof (Data),
             SPACEMIT_TLV_MAC_BASE,
             &Offset
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Data[Offset + 1] != 6) {
    return EFI_COMPROMISED_DATA;
  }

  UINT8  Base[6];

  CopyMem (Base, Data + Offset + 2, sizeof (Base));
  UINT8  Any = 0;

  for (UINTN I = 0; I < sizeof (Base); I++) {
    Any |= Base[I];
  }

  if (!Any || (Base[0] & 1)) {
    return EFI_COMPROMISED_DATA;
  }

  UINTN  Count = 1;

  Status = SpacemitTlvFind (
             Data,
             sizeof (Data),
             SPACEMIT_TLV_MAC_COUNT,
             &Offset
             );
  if (!EFI_ERROR (Status)) {
    if (Data[Offset + 1] != 2) {
      return EFI_COMPROMISED_DATA;
    }

    Count = ((UINTN)Data[Offset + 2] << 8) | Data[Offset + 3];
    if (!Count) {
      return EFI_COMPROMISED_DATA;
    }
  } else if (Status != EFI_NOT_FOUND) {
    return Status;
  }

  if (Index >= Count) {
    return EFI_NOT_FOUND;
  }

  /* Vendor K3 allocation increments the low 24 bits, preserving the
     prefix. Reject wrap rather than reusing an earlier allocated MAC. */
  UINT32  Suffix = ((UINT32)Base[3] << 16) | ((UINT32)Base[4] << 8) |
                   Base[5];

  if (Count - 1 > 0xffffff - Suffix) {
    return EFI_COMPROMISED_DATA;
  }

  Suffix += (UINT32)Index;
  Base[3] = (UINT8)(Suffix >> 16);
  Base[4] = (UINT8)(Suffix >> 8);
  Base[5] = (UINT8)Suffix;
  CopyMem (Mac, Base, sizeof (Base));
  return EFI_SUCCESS;
}
