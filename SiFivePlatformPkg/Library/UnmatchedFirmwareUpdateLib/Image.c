/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Guid/LzmaDecompress.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/ExtractGuidedSectionLib.h>
#include <Library/MemoryAllocationLib.h>
#include "Internal.h"

STATIC UINT32
Le32 (
  CONST VOID  *P
  )
{
  return ReadUnaligned32 (P);
}

STATIC UINT32
Be32 (
  CONST VOID  *P
  )
{
  return SwapBytes32 (Le32 (P));
}

STATIC UINT64
Be64 (
  CONST VOID  *P
  )
{
  return SwapBytes64 (ReadUnaligned64 (P));
}

BOOLEAN
UnmatchedFirmwareMapValid (
  CONST UINT8  *Map
  )
{
  STATIC CONST struct {
    UINT32         Offset, Size;
    CONST CHAR8    *Name;
    UINT16         Flags;
  } Areas[] = {
    { 0,                       SIZE_64KB,             "BOOTBLOCK",       0 },
    { UNMATCHED_CONFIG_OFFSET, UNMATCHED_CONFIG_SIZE, "PLATFORM_CONFIG", 8 },
    { UNMATCHED_FMAP_OFFSET,   UNMATCHED_FMAP_SIZE,   "FMAP",            0 },
    { UNMATCHED_CBFS_OFFSET,   UNMATCHED_CBFS_SIZE,   "COREBOOT",        0 },
    { UNMATCHED_NV_OFFSET,     UNMATCHED_NV_SIZE,     "NVRAM",           8 },
    { 0x1f90000,               0x70000,               "UNUSED",          0 }
  };

  if (CompareMem (Map, "__FMAP__\1\1", 10) || ReadUnaligned64 (Map + 10) ||
      (Le32 (Map + 18) != UNMATCHED_FIRMWARE_SIZE) || CompareMem (Map + 22, "FLASH", 6) ||
      (ReadUnaligned16 (Map + 54) != ARRAY_SIZE (Areas)))
  {
    return FALSE;
  }

  for (UINTN I = 0; I < ARRAY_SIZE (Areas); I++) {
    CONST UINT8  *A = Map + 56 + I * 42;
    if ((Le32 (A) != Areas[I].Offset) || (Le32 (A + 4) != Areas[I].Size) ||
        CompareMem (A + 8, Areas[I].Name, AsciiStrLen (Areas[I].Name) + 1) ||
        (ReadUnaligned16 (A + 40) != Areas[I].Flags))
    {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC BOOLEAN
SpiEntryValid (
  CONST UINT8  *Image
  )
{
  UINT32  Insn = Le32 (Image);
  UINT32  Offset;

  // Memory-mapped SPI starts with JAL x0 to the bootblock. A backward
  // branch would leave the image. GPT is only used by SD-card boot.
  if (((Insn & 0xfff) != 0x6f) || (Insn & BIT31)) {
    return FALSE;
  }

  Offset = (((Insn >> 12) & 0xff) << 12) |
           (((Insn >> 20) & 1) << 11) |
           (((Insn >> 21) & 0x3ff) << 1);
  return (Offset >= sizeof (Insn)) && (Offset < SIZE_64KB);
}

STATIC EFI_STATUS
DecodeConfig (
  CONST UINT8  *Data,
  UINT32       Size,
  UINT32       RawSize,
  VOID         **Result
  )
{
  EFI_GUID_DEFINED_SECTION  *Section;
  VOID                      *Scratch = NULL, *Output = NULL, *Decoded;
  UINT32                    OutputSize, ScratchSize, Authentication;
  UINT16                    Attributes;
  EFI_STATUS                Status;
  UINTN                     Total = sizeof (*Section) + Size;

  *Result = NULL;
  if ((Size < 13) || (Size > SIZE_64KB) || !RawSize || (RawSize > SIZE_64KB) ||
      (ReadUnaligned64 (Data + 5) != RawSize))
  {
    return EFI_COMPROMISED_DATA;
  }

  Section = AllocateZeroPool (Total);
  if (!Section) {
    return EFI_OUT_OF_RESOURCES;
  }

  Section->CommonHeader.Type    = EFI_SECTION_GUID_DEFINED;
  Section->CommonHeader.Size[0] = (UINT8)Total;
  Section->CommonHeader.Size[1] = (UINT8)(Total >> 8);
  Section->CommonHeader.Size[2] = (UINT8)(Total >> 16);
  CopyGuid (&Section->SectionDefinitionGuid, &gLzmaCustomDecompressGuid);
  Section->DataOffset = sizeof (*Section);
  Section->Attributes = EFI_GUIDED_SECTION_PROCESSING_REQUIRED;
  CopyMem (Section + 1, Data, Size);
  Status = ExtractGuidedSectionGetInfo (Section, &OutputSize, &ScratchSize, &Attributes);
  if (EFI_ERROR (Status) || (OutputSize != RawSize) || (ScratchSize > SIZE_1MB)) {
    Status = EFI_COMPROMISED_DATA;
    goto Out;
  }

  Output  = AllocateZeroPool (OutputSize + 1);
  Scratch = AllocatePool (ScratchSize ? ScratchSize : 1);
  if (!Output || !Scratch) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Out;
  }

  Decoded = Output;
  Status  = ExtractGuidedSectionDecode (Section, &Decoded, Scratch, &Authentication);
  if (!EFI_ERROR (Status) && (Decoded != Output)) {
    Status = EFI_COMPROMISED_DATA;
  }

  if (!EFI_ERROR (Status)) {
    *Result = Output;
    Output  = NULL;
  }

Out:
  if (Output) {
    FreePool (Output);
  }

  if (Scratch) {
    FreePool (Scratch);
  }

  FreePool (Section);
  return Status;
}

STATIC BOOLEAN
ConfigValid (
  CONST UINT8  *Data,
  UINTN        Size
  )
{
  BOOLEAN      Board       = FALSE;
  CONST CHAR8  BoardLine[] = "CONFIG_BOARD_SIFIVE_HIFIVE_UNMATCHED=y";
  CONST CHAR8  RamKey[]    = "CONFIG_HIFIVE_UNMATCHED_JTAG_BOOT=";

  if (ScanMem8 (Data, Size, 0)) {
    return FALSE;
  }

  for (UINTN Pos = 0; Pos < Size;) {
    UINTN  End = Pos;
    while (End < Size && Data[End] != '\n') {
      End++;
    }

    UINTN  Length = End - Pos;
    if (Length && (Data[Pos + Length - 1] == '\r')) {
      Length--;
    }

    if ((Length == sizeof (BoardLine) - 1) && !CompareMem (Data + Pos, BoardLine, Length)) {
      if (Board) {
        return FALSE;
      }

      Board = TRUE;
    }

    if ((Length >= sizeof (RamKey) - 1) && !CompareMem (Data + Pos, RamKey, sizeof (RamKey) - 1) &&
        ((Length != sizeof (RamKey)) || (Data[Pos + sizeof (RamKey) - 1] != 'n')))
    {
      return FALSE;
    }

    Pos = End + (End < Size);
  }

  return Board;
}

STATIC BOOLEAN
PayloadValid (
  CONST UINT8  *Data,
  UINT32       Size
  )
{
  // The normal Unmatched payload is one 4 MiB EDK2 segment plus its entry.
  return Size >= 56 && !CompareMem (Data, "CODE", 4) &&
         Be32 (Data + 4) <= 1 && Be32 (Data + 8) == 56 &&
         Be64 (Data + 12) == 0x82200000 && Be32 (Data + 20) == Size - 56 &&
         Be32 (Data + 24) == SIZE_4MB && !CompareMem (Data + 28, "ENTR", 4) &&
         !Be32 (Data + 32) && !Be32 (Data + 36) &&
         Be64 (Data + 40) == 0x82200000 && !Be32 (Data + 48) && !Be32 (Data + 52);
}

EFI_STATUS
UnmatchedFirmwareImageValid (
  CONST VOID  *Buffer,
  UINTN       Size
  )
{
  CONST UINT8  *Image   = Buffer;
  UINT32       Required = 0;

  if (!Image || (Size != UNMATCHED_FIRMWARE_SIZE)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if (!SpiEntryValid (Image) || !UnmatchedFirmwareMapValid (Image + UNMATCHED_FMAP_OFFSET)) {
    return EFI_COMPROMISED_DATA;
  }

  for (UINTN Pos = 0; Pos + 24 <= UNMATCHED_CBFS_SIZE;) {
    CONST UINT8  *File = Image + UNMATCHED_CBFS_OFFSET + Pos;
    if (CompareMem (File, "LARCHIVE", 8)) {
      Pos += 64;
      continue;
    }

    UINT32   Length = Be32 (File + 8), Type = Be32 (File + 12);
    UINT32   Attr = Be32 (File + 16), Offset = Be32 (File + 20);
    UINT32   Compression = 0, RawSize = Length;
    BOOLEAN  Compressed = FALSE;
    if ((Offset < 25) || (Offset > 256) || (Offset > UNMATCHED_CBFS_SIZE - Pos) ||
        (Length > UNMATCHED_CBFS_SIZE - Pos - Offset) ||
        (Attr && ((Attr < 25) || (Attr > Offset))))
    {
      return EFI_COMPROMISED_DATA;
    }

    if (Type && (Type != MAX_UINT32)) {
      UINTN  NameSize = (Attr ? Attr : Offset) - 24;
      if (!ScanMem8 (File + 24, NameSize, 0)) {
        return EFI_COMPROMISED_DATA;
      }

      while (Attr && Attr < Offset) {
        if (Offset - Attr < 8) {
          return EFI_COMPROMISED_DATA;
        }

        UINT32  Tag = Be32 (File + Attr), Bytes = Be32 (File + Attr + 4);
        if (!Tag || (Tag == MAX_UINT32)) {
          break;
        }

        if ((Bytes < 8) || (Bytes > Offset - Attr) || Bytes % 4) {
          return EFI_COMPROMISED_DATA;
        }

        if (Tag == 0x42435a4c) {
          if (Compressed || (Bytes != 16)) {
            return EFI_COMPROMISED_DATA;
          }

          Compressed  = TRUE;
          Compression = Be32 (File + Attr + 8);
          RawSize     = Be32 (File + Attr + 12);
        }

        Attr += Bytes;
      }

      CONST CHAR8  *Name = (CONST CHAR8 *)File + 24;
      CONST UINT8  *Data = File + Offset;
      UINT32       Bit   = 0;
      if (!AsciiStrCmp (Name, "fallback/ramstage")) {
        if ((Type != 0x11) || !Length) {
          return EFI_COMPROMISED_DATA;
        }

        Bit = BIT0;
      }

      if (!AsciiStrCmp (Name, "fallback/opensbi")) {
        if ((Type != 0x20) || !Length) {
          return EFI_COMPROMISED_DATA;
        }

        Bit = BIT1;
      }

      if (!AsciiStrCmp (Name, "fallback/payload")) {
        if ((Type != 0x20) || Compression || !PayloadValid (Data, Length)) {
          return EFI_COMPROMISED_DATA;
        }

        Bit = BIT2;
      }

      if (!AsciiStrCmp (Name, "fallback/DTB")) {
        if ((Type != 0x50) || Compression) {
          return EFI_COMPROMISED_DATA;
        }

        if (Length < 40) {
          return EFI_COMPROMISED_DATA;
        }

        VOID  *Fdt = AllocateCopyPool (Length, Data);
        if (!Fdt) {
          return EFI_OUT_OF_RESOURCES;
        }

        BOOLEAN  Valid = !FdtCheckHeader (Fdt) && FdtTotalSize (Fdt) <= Length &&
                         FdtStringListSearch (Fdt, 0, "compatible", "sifive,hifive-unmatched-a00") >= 0;
        FreePool (Fdt);
        if (!Valid) {
          return EFI_COMPROMISED_DATA;
        }

        Bit = BIT3;
      }

      if (!AsciiStrCmp (Name, "config")) {
        if (Type != 0x50) {
          return EFI_COMPROMISED_DATA;
        }

        VOID        *Config = NULL;
        EFI_STATUS  Status;
        if (Compression == 1) {
          Status = DecodeConfig (Data, Length, RawSize, &Config);
        } else if (!Compression && Length && (Length <= SIZE_64KB)) {
          Config = AllocateCopyPool (Length, Data);
          Status = Config ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
        } else {
          Status = EFI_COMPROMISED_DATA;
        }

        if (EFI_ERROR (Status)) {
          return Status;
        }

        BOOLEAN  Valid = ConfigValid (Config, Compression ? RawSize : Length);
        FreePool (Config);
        if (!Valid) {
          return EFI_UNSUPPORTED;
        }

        Bit = BIT4;
      }

      if (Bit & Required) {
        return EFI_COMPROMISED_DATA;
      }

      Required |= Bit;
    }

    Pos = ALIGN_VALUE (Pos + Offset + Length, 64);
  }

  return Required == (BIT0 | BIT1 | BIT2 | BIT3 | BIT4) ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
}
