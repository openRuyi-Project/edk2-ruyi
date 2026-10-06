/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/SpacemitTlvLib.h>

EFI_STATUS
SpacemitTlvValidate (
  CONST UINT8  *Data,
  UINTN        Capacity,
  UINTN        *Used
  )
{
  STATIC CONST UINT8  Signature[8] = { 'T', 'l', 'v', 'I', 'n', 'f', 'o', 0 };

  if (!Data || !Used || (Capacity < 17)) {
    return EFI_INVALID_PARAMETER;
  }

  if (CompareMem (Data, Signature, 8) || (Data[8] != 1)) {
    return EFI_VOLUME_CORRUPTED;
  }

  UINTN  End = 11 + ((UINTN)Data[9] << 8) + Data[10];

  if ((End < 17) || (End > Capacity) || (End > 2048)) {
    return EFI_VOLUME_CORRUPTED;
  }

  UINTN  Offset = 11;

  while (Offset < End) {
    if ((End - Offset < 2) || (Data[Offset] == 0) || (Data[Offset] == 0xff) ||
        (Data[Offset + 1] > End - Offset - 2))
    {
      return EFI_VOLUME_CORRUPTED;
    }

    UINTN  Next = Offset + 2 + Data[Offset + 1];
    if (Data[Offset] == SPACEMIT_TLV_CRC) {
      if ((Data[Offset + 1] != 4) || (Next != End)) {
        return EFI_VOLUME_CORRUPTED;
      }

      UINT32  Crc = ((UINT32)Data[End - 4] << 24) |
                    ((UINT32)Data[End - 3] << 16) |
                    ((UINT32)Data[End - 2] << 8) | Data[End - 1];
      if (Crc != CalculateCrc32 ((VOID *)Data, End - 4)) {
        return EFI_CRC_ERROR;
      }

      *Used = End;
      return EFI_SUCCESS;
    }

    Offset = Next;
  }

  return EFI_VOLUME_CORRUPTED;
}

EFI_STATUS
SpacemitTlvFind (
  CONST UINT8  *Data,
  UINTN        Capacity,
  UINT8        Code,
  UINTN        *Offset
  )
{
  if (!Offset || (Code == 0) || (Code == 0xff)) {
    return EFI_INVALID_PARAMETER;
  }

  UINTN       Used;
  EFI_STATUS  Status = SpacemitTlvValidate (Data, Capacity, &Used);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  BOOLEAN  Found = FALSE;

  for (UINTN Pos = 11; Pos < Used; Pos += 2 + Data[Pos + 1]) {
    if (Data[Pos] == Code) {
      if (Found) {
        return EFI_VOLUME_CORRUPTED;
      }

      *Offset = Pos;
      Found   = TRUE;
    }
  }

  return Found ? EFI_SUCCESS : EFI_NOT_FOUND;
}

EFI_STATUS
SpacemitTlvSet (
  UINT8        *Data,
  UINTN        Capacity,
  UINT8        Code,
  CONST UINT8  *Value,
  UINTN        Length
  )
{
  if ((Code == 0) || (Code >= SPACEMIT_TLV_CRC) || (Length > 255) ||
      (!Value && Length))
  {
    return EFI_INVALID_PARAMETER;
  }

  UINTN       Used, Offset;
  EFI_STATUS  Status = SpacemitTlvValidate (Data, Capacity, &Used);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SpacemitTlvFind (Data, Capacity, Code, &Offset);
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
    return Status;
  }

  UINTN  Old = Status == EFI_NOT_FOUND ? 0 : 2 + Data[Offset + 1];
  UINTN  New = Value ? 2 + Length : 0;

  if (!Old) {
    if (!Value) {
      return EFI_NOT_FOUND;
    }

    Offset = Used - 6;
  }

  if ((New > Capacity - (Used - Old)) || (Used - Old + New > 2048)) {
    return EFI_BUFFER_TOO_SMALL;
  }

  /* Preserve overlapping input before moving the following opaque TLVs. */
  UINT8  Saved[255];

  if (Length) {
    CopyMem (Saved, Value, Length);
  }

  CopyMem (Data + Offset + New, Data + Offset + Old, Used - Offset - Old);
  if (Value) {
    Data[Offset]     = Code;
    Data[Offset + 1] = (UINT8)Length;
    CopyMem (Data + Offset + 2, Saved, Length);
  }

  Used     = Used - Old + New;
  Data[9]  = (UINT8)((Used - 11) >> 8);
  Data[10] = (UINT8)(Used - 11);
  UINT32  Crc = CalculateCrc32 (Data, Used - 4);

  for (UINTN I = 0; I < 4; I++) {
    Data[Used - 4 + I] = (UINT8)(Crc >> (24 - I * 8));
  }

  return EFI_SUCCESS;
}
