/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CorebootImageLib.h>
#include "Internal.h"

STATIC UINT32
Le32 (
  CONST VOID  *P
  )
{
  return ReadUnaligned32 (P);
}

BOOLEAN
UnmatchedFirmwareMapValid (
  CONST UINT8  *Map,
  UINT32       FirmwareSize
  )
{
  CONST struct {
    UINT32         Offset, Size;
    CONST CHAR8    *Name;
    UINT16         Flags;
  } Areas[] = {
    { 0,                       SIZE_64KB,             "BOOTBLOCK",       0 },
    { UNMATCHED_CONFIG_OFFSET, UNMATCHED_CONFIG_SIZE, "PLATFORM_CONFIG", 8 },
    { UNMATCHED_FMAP_OFFSET,   UNMATCHED_FMAP_SIZE,   "FMAP",            0 },
    { UNMATCHED_CBFS_OFFSET,
      FirmwareSize == SIZE_32MB ? UNMATCHED_NV_OFFSET - UNMATCHED_CBFS_OFFSET : UNMATCHED_CBFS_SIZE,
      "COREBOOT", 0 },
    { UNMATCHED_NV_OFFSET,     UNMATCHED_NV_SIZE,     "NVRAM",           8 },
    { UNMATCHED_NV_OFFSET + UNMATCHED_NV_SIZE,
      SIZE_32MB - UNMATCHED_NV_OFFSET - UNMATCHED_NV_SIZE, "UNUSED", 0 }
  };
  // The legacy 32 MiB map includes the variable store and trailing space.
  UINTN  AreaCount = FirmwareSize == SIZE_32MB ? ARRAY_SIZE (Areas) : ARRAY_SIZE (Areas) - 2;

  if (((FirmwareSize != UNMATCHED_FIRMWARE_SIZE) && (FirmwareSize != SIZE_32MB)) ||
      CompareMem (Map, "__FMAP__\1\1", 10) || ReadUnaligned64 (Map + 10) ||
      (Le32 (Map + 18) != FirmwareSize) || CompareMem (Map + 22, "FLASH", 6) ||
      (ReadUnaligned16 (Map + 54) != AreaCount))
  {
    return FALSE;
  }

  for (UINTN I = 0; I < AreaCount; I++) {
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

EFI_STATUS
UnmatchedFirmwareImageValid (CONST VOID *Buffer, UINTN Size)
{
  CONST UINT8 *Image = Buffer;
  CONST COREBOOT_IMAGE_LAYOUT Layout = {
    "sifive,hifive-unmatched-a00", "CONFIG_BOARD_SIFIVE_HIFIVE_UNMATCHED=y",
    "CONFIG_HIFIVE_UNMATCHED_JTAG_BOOT=", 0x82200000, SIZE_4MB
  };
  if (!Image || Size != UNMATCHED_FIRMWARE_SIZE) { return EFI_BAD_BUFFER_SIZE; }
  if (!SpiEntryValid (Image) || !UnmatchedFirmwareMapValid (Image + UNMATCHED_FMAP_OFFSET, UNMATCHED_FIRMWARE_SIZE)) {
    return EFI_COMPROMISED_DATA;
  }
  return CorebootImageValidate (Image + UNMATCHED_CBFS_OFFSET, UNMATCHED_CBFS_SIZE, &Layout);
}
