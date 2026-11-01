/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/FirmwareUpdateLib.h>

STATIC INTN
Hex (
  CHAR8  C
  )
{
  if ((C >= '0') && (C <= '9')) {
    return C - '0';
  }

  if ((C >= 'a') && (C <= 'f')) {
    return C - 'a' + 10;
  }

  if ((C >= 'A') && (C <= 'F')) {
    return C - 'A' + 10;
  }

  return -1;
}

EFI_STATUS
FirmwareUpdateVerifyChecksum (
  CONST VOID    *Image,
  UINTN         Size,
  CONST CHAR8   *List,
  UINTN         ListSize,
  CONST CHAR16  *Name
  )
{
  UINT8    Expected[SHA256_DIGEST_SIZE], Actual[SHA256_DIGEST_SIZE];
  BOOLEAN  Found = FALSE;

  if (!Image || !List || !Name) {
    return EFI_INVALID_PARAMETER;
  }

  UINTN  NameLength = StrLen (Name);

  for (UINTN Pos = 0; Pos < ListSize;) {
    UINTN  End = Pos;
    while (End < ListSize && List[End] != '\n') {
      End++;
    }

    UINTN  Length = End - Pos;
    if (Length && (List[Pos + Length - 1] == '\r')) {
      Length--;
    }

    if ((Length == 66 + NameLength) && (List[Pos + 64] == ' ') &&
        ((List[Pos + 65] == ' ') || (List[Pos + 65] == '*')))
    {
      BOOLEAN  Match = TRUE;
      for (UINTN I = 0; I < NameLength; I++) {
        if ((Name[I] > 0x7f) || (List[Pos + 66 + I] != (CHAR8)Name[I])) {
          Match = FALSE;
          break;
        }
      }

      if (Match) {
        if (Found) {
          return EFI_COMPROMISED_DATA;
        }

        for (UINTN I = 0; I < sizeof (Expected); I++) {
          INTN  High = Hex (List[Pos + I * 2]);
          INTN  Low  = Hex (List[Pos + I * 2 + 1]);
          if ((High < 0) || (Low < 0)) {
            return EFI_COMPROMISED_DATA;
          }

          Expected[I] = (UINT8)((High << 4) | Low);
        }

        Found = TRUE;
      }
    }

    Pos = End + (End < ListSize);
  }

  if (!Found) {
    return EFI_NOT_FOUND;
  }

  if (!Sha256HashAll (Image, Size, Actual)) {
    return EFI_DEVICE_ERROR;
  }

  return CompareMem (Actual, Expected, sizeof (Actual)) ? EFI_CRC_ERROR : EFI_SUCCESS;
}
