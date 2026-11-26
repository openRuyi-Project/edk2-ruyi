/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/BaseCryptLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include "Internal.h"

#define FHT_MAGIC        0x42575345U
#define SIGNATURE_MAGIC  0xbb455357U
#define ENTRY_BYTES      36U
#define SIGNATURE_BYTES  256U
#define SCPU_ADDRESS     0x59000000ULL
#define MCPU_ADDRESS     0x80000000ULL
#define D2D_ENTRY_BYTES  28U

EFI_STATUS
D560FirmwarePair (
  CONST VOID *Buffer, UINTN Size, CONST EIC7700_BOARD_PROFILE *Board,
  D560_FIRMWARE_DIE Dies[D560_DIE_COUNT]
  )
{
  STATIC CONST UINT32 Limits[] = { D560_DIE0_STAGE_LIMIT, D560_DIE1_STAGE_LIMIT };
  STATIC CONST UINT8 Kinds[] = { 0x70, 0x10, 0x20, 0x30 };
  CONST UINT8 *Bundle = Buffer;
  UINTN Position = SIZE_4KB;

  if (Dies == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (Board == NULL || !Board->DualDie) {
    return EFI_UNSUPPORTED;
  }
  if (Bundle == NULL || Size != D560_UPDATE_BYTES) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (CompareMem (Bundle, "D560SPI", 8) != 0 ||
      ReadUnaligned32 ((CONST UINT32 *)(Bundle + 8)) != 1) {
    return EFI_COMPROMISED_DATA;
  }

  for (UINTN Die = 0; Die < D560_DIE_COUNT; ++Die) {
    // Each descriptor contains a little-endian length followed by its SHA-256.
    CONST UINT8 *Descriptor = Bundle + 12 + Die * (sizeof (UINT32) + SHA256_DIGEST_SIZE);
    UINT32 Length = ReadUnaligned32 ((CONST UINT32 *)Descriptor);
    UINT8 Digest[SHA256_DIGEST_SIZE];
    UINTN Count = ARRAY_SIZE (Kinds) - Die;
    CONST UINT8 *Image = Bundle + Position;
    UINTN Next = D560_STAGE_START;

    if (Length <= Next || Length > Limits[Die] || Length % SIZE_4KB != 0 ||
        Length > Size - Position) {
      return EFI_COMPROMISED_DATA;
    }
    if (!Sha256HashAll (Image, Length, Digest) ||
        CompareMem (Digest, Descriptor + sizeof (UINT32), sizeof (Digest)) != 0) {
      return EFI_CRC_ERROR;
    }
    if (ReadUnaligned32 ((CONST UINT32 *)Image) != FHT_MAGIC ||
        ReadUnaligned32 ((CONST UINT32 *)(Image + 4)) != Count) {
      return EFI_COMPROMISED_DATA;
    }
    for (UINTN Index = 0; Index < Count; ++Index) {
      CONST UINT8 *Entry = Image + 8 + Index * ENTRY_BYTES;
      UINT64 Offset = ReadUnaligned64 ((CONST UINT64 *)(Entry + 4));
      UINT64 Bytes = ReadUnaligned64 ((CONST UINT64 *)(Entry + 12));
      BOOLEAN Mcpu = Index == 3;

      if (ReadUnaligned32 ((CONST UINT32 *)Entry) != 1 || Entry[20] || Entry[21] ||
          Entry[22] != Kinds[Index] || Entry[23] != (Index + 1 == Count) ||
          ReadUnaligned32 ((CONST UINT32 *)(Entry + 24)) != 0 ||
          Offset != Next || Next > Length - SIGNATURE_BYTES ||
          Bytes == 0 || Bytes > Length - Next - SIGNATURE_BYTES) {
        return EFI_COMPROMISED_DATA;
      }

      CONST UINT8 *Header = Image + Next;
      UINT64 Load = (Index == 0) ? 0x30008000U : (Mcpu ? MCPU_ADDRESS : SCPU_ADDRESS + Die * 0x20000000U);
      UINT64 Start = Load;
      if (Index == 2) {
        // The D2D payload enters through the seven-instruction board adapter.
        if (Bytes <= D2D_ENTRY_BYTES) {
          return EFI_COMPROMISED_DATA;
        }
        Start += Bytes - D2D_ENTRY_BYTES;
      }
      // The USB container offset in Header is retained when its FHT entries
      // are relocated for SPI. The flash offsets come from Entry.
      if (ReadUnaligned32 ((CONST UINT32 *)Header) != SIGNATURE_MAGIC ||
          ReadUnaligned64 ((CONST UINT64 *)(Header + 24)) != Bytes ||
          ReadUnaligned64 ((CONST UINT64 *)(Header + 32)) != Load ||
          ReadUnaligned64 ((CONST UINT64 *)(Header + 40)) != Start ||
          Header[48] != 0 || Header[49] != Mcpu || Header[50] != 0 ||
          Header[64] != Kinds[Index] || Header[65] != Mcpu) {
        return EFI_COMPROMISED_DATA;
      }
      if (Mcpu) {
        CONST UINT8 *Rom = Header + SIGNATURE_BYTES;
        EFI_STATUS Status = Eic7700FirmwareRomValid (Rom, (UINTN)Bytes, &Board->Image);
        if (EFI_ERROR (Status)) {
          return Status;
        }
      }
      Next = ALIGN_VALUE (Next + SIGNATURE_BYTES + (UINTN)Bytes, SIZE_4KB);
    }
    if (Next != Length) {
      return EFI_COMPROMISED_DATA;
    }
    Dies[Die].Image = Image;
    Dies[Die].Size = Length;
    Dies[Die].TableBytes = 8 + Count * ENTRY_BYTES;
    Position += Length;
  }
  return EFI_SUCCESS;
}
