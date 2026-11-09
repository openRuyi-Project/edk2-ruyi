/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CorebootImageLib.h>
#include <Library/PremierP550NvFlashLib.h>
#include "Internal.h"

#define FHT_MAGIC        0x42575345U
#define SIGNATURE_MAGIC  0xbb455357U
#define ENTRY_BYTES      36U
#define SIGNATURE_BYTES  256U
#define ROM_BYTES        SIZE_4MB
#define FMAP_OFFSET      0x20000U
#define CBFS_OFFSET      0x21000U
#define CBFS_BYTES       (ROM_BYTES - CBFS_OFFSET)
#define SCPU_ADDRESS     0x59000000ULL
#define MCPU_ADDRESS     0x90000000ULL

STATIC BOOLEAN
MapValid (CONST UINT8 *Map)
{
  STATIC CONST struct { UINT32 Offset, Bytes; CONST CHAR8 *Name; } Areas[] = {
    {0, 0x20000, "BOOTBLOCK"}, {FMAP_OFFSET, SIZE_4KB, "FMAP"},
    {CBFS_OFFSET, CBFS_BYTES, "COREBOOT"}
  };
  UINTN Index;
  CONST UINT8 *Area;

  if (CompareMem (Map, "__FMAP__\1\1", 10) || ReadUnaligned64 ((CONST UINT64 *)(Map + 10)) ||
      ReadUnaligned32 ((CONST UINT32 *)(Map + 18)) != ROM_BYTES ||
      CompareMem (Map + 22, "FLASH", 6) || ReadUnaligned16 ((CONST UINT16 *)(Map + 54)) != ARRAY_SIZE (Areas)) {
    return FALSE;
  }
  for (Index = 0; Index < ARRAY_SIZE (Areas); Index++) {
    Area = Map + 56 + Index * 42;
    if (ReadUnaligned32 ((CONST UINT32 *)Area) != Areas[Index].Offset ||
        ReadUnaligned32 ((CONST UINT32 *)(Area + 4)) != Areas[Index].Bytes ||
        CompareMem (Area + 8, Areas[Index].Name, AsciiStrLen (Areas[Index].Name) + 1) ||
        ReadUnaligned16 ((CONST UINT16 *)(Area + 40))) { return FALSE; }
  }
  return TRUE;
}

EFI_STATUS
PremierP550FirmwareImageValid (CONST VOID *Buffer, UINTN Size)
{
  CONST UINT8 *Image = Buffer, *Entry, *Header, *Rom = NULL;
  STATIC CONST UINT8 Kinds[] = {0x70, 0x10, 0x30};
  CONST COREBOOT_IMAGE_LAYOUT Layout = {
    "sifive,hifive-premier-p550", "CONFIG_BOARD_SIFIVE_HIFIVE_PREMIER_P550=y",
    NULL, 0x82200000, SIZE_4MB
  };
  UINT64 Offset, Bytes, Address, End = 8 + ARRAY_SIZE (Kinds) * ENTRY_BYTES;
  UINTN Index;

  if (!Image || Size != P550_SPI_IMAGE_BYTES) { return EFI_BAD_BUFFER_SIZE; }
  if (ReadUnaligned32 ((CONST UINT32 *)Image) != FHT_MAGIC ||
      ReadUnaligned32 ((CONST UINT32 *)(Image + 4)) != ARRAY_SIZE (Kinds)) {
    return EFI_COMPROMISED_DATA;
  }
  for (Index = 0; Index < ARRAY_SIZE (Kinds); Index++) {
    Entry = Image + 8 + Index * ENTRY_BYTES;
    Offset = ReadUnaligned64 ((CONST UINT64 *)(Entry + 4));
    Bytes = ReadUnaligned64 ((CONST UINT64 *)(Entry + 12));
    if (ReadUnaligned32 ((CONST UINT32 *)Entry) != 1 || Entry[20] || Entry[21] ||
        Entry[22] != Kinds[Index] || Entry[23] != (Index == 2) ||
        ReadUnaligned32 ((CONST UINT32 *)(Entry + 24)) ||
        ReadUnaligned64 ((CONST UINT64 *)(Entry + 28)) ||
        Offset < End || (Offset % 32) || Offset >= P550_FIRMWARE_BYTES ||
        Bytes > P550_FIRMWARE_BYTES - Offset ||
        SIGNATURE_BYTES > P550_FIRMWARE_BYTES - Offset - Bytes || !Bytes) {
      return EFI_COMPROMISED_DATA;
    }
    Header = Image + Offset;
    Address = Index == 2 ? MCPU_ADDRESS : SCPU_ADDRESS;
    if (ReadUnaligned32 ((CONST UINT32 *)Header) != SIGNATURE_MAGIC ||
        ReadUnaligned64 ((CONST UINT64 *)(Header + 16)) != Offset ||
        ReadUnaligned64 ((CONST UINT64 *)(Header + 24)) != Bytes ||
        ReadUnaligned64 ((CONST UINT64 *)(Header + 32)) != Address ||
        ReadUnaligned64 ((CONST UINT64 *)(Header + 40)) != Address ||
        Header[48] || Header[49] || Header[50] || Header[64] != Kinds[Index] ||
        Header[65] != (Index == 2)) { return EFI_COMPROMISED_DATA; }
    if (Index == 2) {
      if (Bytes != ROM_BYTES) { return EFI_COMPROMISED_DATA; }
      Rom = Header + SIGNATURE_BYTES;
    }
    End = Offset + SIGNATURE_BYTES + Bytes;
  }
  if (!MapValid (Rom + FMAP_OFFSET)) { return EFI_COMPROMISED_DATA; }
  return CorebootImageValidate (Rom + CBFS_OFFSET, CBFS_BYTES, &Layout);
}
