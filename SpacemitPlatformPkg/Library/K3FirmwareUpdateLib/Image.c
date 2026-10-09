/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/FdtLib.h>
#include <Library/K3FirmwareUpdateLib.h>
#include <Library/MemoryAllocationLib.h>
#include "Internal.h"

#define CBFS_OFFSET  0x210000U
#define CBFS_SIZE    0x300000U
#define BOOT_OFFSET  0x20000U

STATIC UINT32
Le32 (
  CONST UINT8  *P
  )
{
  return ReadUnaligned32 (P);
}

STATIC UINT32
Be32 (
  CONST UINT8  *P
  )
{
  return SwapBytes32 (Le32 (P));
}

STATIC BOOLEAN
CrcMatches (
  CONST UINT8  *Data,
  UINTN        Size,
  CONST UINT8  *Expected
  )
{
  return CalculateCrc32 ((VOID *)Data, Size) == Le32 (Expected);
}

STATIC BOOLEAN
HashMatches (
  CONST UINT8  *Data,
  UINTN        Size,
  CONST UINT8  *Expected
  )
{
  UINT8  Digest[SHA256_DIGEST_SIZE];

  return Sha256HashAll (Data, Size, Digest) &&
         !CompareMem (Digest, Expected, sizeof (Digest));
}

BOOLEAN
K3FirmwareMapValid (
  CONST UINT8  *Map
  )
{
  STATIC CONST struct {
    UINT32         Offset, Size;
    CONST CHAR8    *Name;
    UINT16         Flags;
  } Areas[] = {
    { 0,                     0x20000,             "BOOTINFO",       8 },
    { 0x20000,               0x80000,             "BOOTBLOCK",      0 },
    { 0xa0000,               0x10000,             "ENVIRONMENT",    8 },
    { 0xb0000,               0x100000,            NULL,             8 },
    { 0x1b0000,              0x1000,              "FMAP",           0 },
    { 0x1b1000,              0x5f000,             "EARLY_RESERVED", 8 },
    { 0x210000,              0x300000,            "COREBOOT",       0 },
    { 0x510000,              0x1f0000,            "RESERVED",       8 },
    { K3_FIRMWARE_NV_OFFSET, K3_FIRMWARE_NV_SIZE, "UEFI_VARIABLES", 8 },
    { 0x7c0000,              0x40000,             "TAIL",           8 }
  };

  if (CompareMem (Map, "__FMAP__\1\1", 10) ||
      (ReadUnaligned64 ((CONST UINT64 *)(Map + 10)) != 0) ||
      (Le32 (Map + 18) != K3_FIRMWARE_SIZE) ||
      (ReadUnaligned16 ((CONST UINT16 *)(Map + 54)) != ARRAY_SIZE (Areas)))
  {
    return FALSE;
  }

  for (UINTN I = 0; I < ARRAY_SIZE (Areas); I++) {
    CONST UINT8  *A = Map + 56 + I * 42;
    if ((Le32 (A) != Areas[I].Offset) || (Le32 (A + 4) != Areas[I].Size) ||
        (Areas[I].Name && CompareMem (
                            A + 8,
                            Areas[I].Name,
                            AsciiStrLen (Areas[I].Name) + 1
                            )) ||
        (ReadUnaligned16 ((CONST UINT16 *)(A + 40)) != Areas[I].Flags))
    {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC BOOLEAN
CheckCbfs (
  CONST UINT8  *Image,
  VOID         *Context,
  CONST UINT8  *Expected
  )
{
  CONST UINT8  *Cbfs    = Image + CBFS_OFFSET;
  UINT32       Required = 0;
  UINT8        Digest[SHA256_DIGEST_SIZE];

  if (!Sha256Init (Context)) {
    return FALSE;
  }

  for (UINTN Pos = 0; Pos + 24 <= CBFS_SIZE;) {
    CONST UINT8  *File = Cbfs + Pos;
    if (CompareMem (File, "LARCHIVE", 8)) {
      Pos += 64;
      continue;
    }

    UINT32  Length = Be32 (File + 8), Type = Be32 (File + 12);
    UINT32  Attr = Be32 (File + 16), Offset = Be32 (File + 20);
    if ((Offset < 25) || (Offset > 256) || (Offset > CBFS_SIZE - Pos) ||
        (Length > CBFS_SIZE - Pos - Offset) ||
        (Attr && ((Attr < 25) || (Attr > Offset))))
    {
      return FALSE;
    }

    if (Type && (Type != MAX_UINT32)) {
      UINTN  NameEnd = Attr ? Attr : Offset;
      if ((File[NameEnd - 1] != 0) || !Sha256Update (Context, File, Offset)) {
        return FALSE;
      }

      BOOLEAN  HasHash = FALSE;
      while (Attr && Attr < Offset) {
        if (Offset - Attr < 8) {
          return FALSE;
        }

        UINT32  Tag = Be32 (File + Attr), Size = Be32 (File + Attr + 4);
        if (!Tag || (Tag == MAX_UINT32)) {
          break;
        }

        if ((Size < 8) || (Size > Offset - Attr) || Size % 4) {
          return FALSE;
        }

        if (Tag == 0x68736148) {
          // CBFS SHA-256 hash attribute.
          if (HasHash || (Size != 44) || (Be32 (File + Attr + 8) != 2) ||
              !HashMatches (File + Offset, Length, File + Attr + 12))
          {
            return FALSE;
          }

          HasHash = TRUE;
        }

        Attr += Size;
      }

      // Legacy CBFS master-header records do not carry file hashes.
      if (!HasHash && (Type != 2)) {
        return FALSE;
      }

      CONST CHAR8  *Name = (CONST CHAR8 *)File + 24;
      if (!AsciiStrCmp (Name, "fallback/ramstage") && (Type == 0x11)) {
        Required |= BIT0;
      }

      if (!AsciiStrCmp (Name, "fallback/opensbi") && (Type == 0x20)) {
        Required |= BIT1;
      }

      if (!AsciiStrCmp (Name, "fallback/payload") && (Type == 0x20)) {
        Required |= BIT2;
      }

      if (!AsciiStrCmp (Name, "k3-pico-itx.dtb") && (Type == 0x50)) {
        VOID  *Fdt = AllocateCopyPool (Length, File + Offset);
        if (!Fdt) {
          return FALSE;
        }

        BOOLEAN  Valid = Length >= 40 && !FdtCheckHeader (Fdt) && FdtTotalSize (Fdt) <= Length &&
                         FdtStringListSearch (Fdt, 0, "compatible", "spacemit,k3-pico-itx") >= 0;
        FreePool (Fdt);
        if (!Valid) {
          return FALSE;
        }

        Required |= BIT3;
      }
    }

    Pos = ALIGN_VALUE (Pos + Offset + Length, 64);
  }

  return Required == (BIT0 | BIT1 | BIT2 | BIT3) && Sha256Final (Context, Digest) &&
         !CompareMem (Digest, Expected, sizeof (Digest));
}

EFI_STATUS
K3FirmwareValidate (
  CONST VOID  *Buffer,
  UINTN       Size
  )
{
  CONST UINT8  *Image = Buffer;
  CONST UINT8  *First, *Header, *Anchor = NULL;
  UINT64       Length;

  if (!Buffer || (Size != K3_FIRMWARE_SIZE)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if ((Le32 (Image) != 0xb00714f0) || (Le32 (Image + 4) != 0x10001) ||
      CompareMem (Image + 8, "NORF", 4) || (Le32 (Image + 16) != 256) ||
      (Le32 (Image + 20) != SIZE_64KB) || (Le32 (Image + 24) != Size) ||
      (Le32 (Image + 32) != BOOT_OFFSET) || (Le32 (Image + 40) != 0x74000) ||
      !CrcMatches (Image, 64, Image + 64) || !K3FirmwareMapValid (Image + FMAP_OFFSET))
  {
    return EFI_COMPROMISED_DATA;
  }

  First  = Image + BOOT_OFFSET + 0x100;
  Header = Image + BOOT_OFFSET + 0xfe0;
  Length = ReadUnaligned64 ((CONST UINT64 *)(Header + 8));
  if (CompareMem (First, "AIHD\1\0\0\0", 8) ||
      (ReadUnaligned64 ((CONST UINT64 *)(First + 8)) != 0xfe0) ||
      (ReadUnaligned64 ((CONST UINT64 *)(First + 16)) != 0xc0800000) ||
      !CrcMatches (First, 24, First + 24) ||
      CompareMem (Header, "AIHD\1\0\0\0", 8) || !Length || Length % 32 ||
      (Length > 0x74000 - 0x1100) ||
      (ReadUnaligned64 ((CONST UINT64 *)(Header + 16)) != 0) ||
      !CrcMatches (Header, 24, Header + 24) ||
      !CrcMatches (Header + 32, (UINTN)Length, Header + 28))
  {
    return EFI_COMPROMISED_DATA;
  }

  // The CRC-covered bootblock anchors the CBFS metadata and FMAP hashes.
  for (UINTN I = 0; I + 76 <= Length; I++) {
    CONST UINT8  *P = Header + 32 + I;
    if (!CompareMem (P, "\xadMdtHsh\x15", 8)) {
      if (Anchor || (Be32 (P + 8) != 2)) {
        return EFI_COMPROMISED_DATA;
      }

      Anchor = P;
    }
  }

  if (!Anchor || !HashMatches (Image + FMAP_OFFSET, FMAP_SIZE, Anchor + 44)) {
    return EFI_COMPROMISED_DATA;
  }

  VOID  *Context = AllocatePool (Sha256GetContextSize ());

  if (!Context) {
    return EFI_OUT_OF_RESOURCES;
  }

  BOOLEAN  Valid = CheckCbfs (Image, Context, Anchor + 12);

  FreePool (Context);
  return Valid ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
}
