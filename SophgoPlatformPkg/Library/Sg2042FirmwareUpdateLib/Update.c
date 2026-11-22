/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CorebootImageLib.h>
#include <Library/CpuMmioAddressLib.h>
#include <Library/FdtBoardInfoLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NvFvbLib.h>
#include <Library/PlatformFirmwareUpdateLib.h>
#include <Library/Sg2042SpiNorLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define FIP_OFFSET    0x30000U
#define FIP_MAGIC     0xaa640001U
#define ZSBL_OFFSET   SIZE_8MB
#define FMAP_OFFSET   0x20000U
#define CBFS_OFFSET   0x21000U
#define SCP_ENTRY_SIZE 56U

STATIC
BOOLEAN
EntryMatches (
  CONST UINT8 *Entry, CONST CHAR8 *Name, UINT32 Offset, UINT32 Size, UINT64 LoadAddress
  )
{
  return ReadUnaligned32 ((CONST VOID *)Entry) == SG2042_SCP_MAGIC &&
         !CompareMem (Entry + 4, Name, AsciiStrLen (Name) + 1) &&
         ReadUnaligned32 ((CONST VOID *)(Entry + 36)) == Offset &&
         ReadUnaligned32 ((CONST VOID *)(Entry + 40)) == Size &&
         ReadUnaligned64 ((CONST VOID *)(Entry + 48)) == LoadAddress;
}

STATIC EFI_STATUS
ValidateImage (CONST UINT8 *Image, UINTN Size)
{
  STATIC CONST struct { CONST CHAR8 *Name; CONST CHAR8 *Compatible; } Boards[] = {
    { "sg2042-evb-v1.dtb", "sophgo,sg2042-evb-v1" },
    { "sg2042-milkv-pioneer.dtb", "milkv,pioneer" },
    { "sg2042-evb-v2.dtb", "sophgo,sg2042-evb-v2" }
  };
  if (Image == NULL || Size != SIZE_16MB) { return EFI_BAD_BUFFER_SIZE; }
  CONST UINT8 *Table = Image + SG2042_SCP_TABLE;
  UINT32 FipSize = ReadUnaligned32 ((CONST VOID *)(Table + 40));
  CONST UINT8 *Rom = Image + ZSBL_OFFSET;
  CONST UINT8 *Map = Rom + FMAP_OFFSET;
  if (FipSize < 16 || FipSize > SG2042_SCP_TABLE - FIP_OFFSET ||
      ReadUnaligned32 ((CONST VOID *)(Image + FIP_OFFSET)) != FIP_MAGIC ||
      !EntryMatches (Table, "fip.bin", FIP_OFFSET, FipSize, 0) ||
      !EntryMatches (Table + SCP_ENTRY_SIZE, "ruyi-vars", SG2042_VARIABLE_BASE, NV_TOTAL_SIZE, 0) ||
      !EntryMatches (Table + 2 * SCP_ENTRY_SIZE, "zsbl.bin", ZSBL_OFFSET, SIZE_8MB, 0x40000000) ||
      ReadUnaligned32 ((CONST VOID *)(Table + 3 * SCP_ENTRY_SIZE)) != MAX_UINT32 ||
      CompareMem (Map, "__FMAP__\1\1", 10) || ReadUnaligned64 ((CONST VOID *)(Map + 10)) != 0 ||
      ReadUnaligned32 ((CONST VOID *)(Map + 18)) != SIZE_8MB || CompareMem (Map + 22, "FLASH", 6) ||
      ReadUnaligned16 ((CONST VOID *)(Map + 54)) != 3) {
    return EFI_COMPROMISED_DATA;
  }

  STATIC CONST struct { UINT32 Offset; UINT32 Size; CONST CHAR8 *Name; } Areas[] = {
    { 0, FMAP_OFFSET, "BOOTBLOCK" },
    { FMAP_OFFSET, CBFS_OFFSET - FMAP_OFFSET, "FMAP" },
    { CBFS_OFFSET, SIZE_8MB - CBFS_OFFSET, "COREBOOT" }
  };
  for (UINTN I = 0; I < ARRAY_SIZE (Areas); I++) {
    CONST UINT8 *Area = Map + 56 + 42 * I;
    if (ReadUnaligned32 ((CONST VOID *)Area) != Areas[I].Offset ||
        ReadUnaligned32 ((CONST VOID *)(Area + 4)) != Areas[I].Size ||
        CompareMem (Area + 8, Areas[I].Name, AsciiStrLen (Areas[I].Name) + 1)) {
      return EFI_COMPROMISED_DATA;
    }
  }

  for (UINTN I = 0; I < ARRAY_SIZE (Boards); I++) {
    COREBOOT_IMAGE_LAYOUT Layout = {
      Boards[I].Compatible, "CONFIG_BOARD_SOPHGO_SG2042=y", NULL, 0x02000000, SIZE_8MB
    };
    EFI_STATUS Status = CorebootImageValidateDtb (Rom + CBFS_OFFSET, SIZE_8MB - CBFS_OFFSET, &Layout, Boards[I].Name);
    if (EFI_ERROR (Status)) { return Status; }
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
GetBootFlash (SG2042_SPI_NOR *Nor)
{
  CONST VOID *Fdt;
  RUYI_BOARD_INFO Info;
  UINT8 Id[3];
  INT32 Node = -1;
  EFI_STATUS Status = FdtBoardInfoGet (&Info, &Fdt);
  if (EFI_ERROR (Status)) { return Status; }
  while ((Node = FdtNodeOffsetByCompatible (Fdt, Node, "sophgo,sg2042-spifmc-nor")) >= 0) {
    if (!EFI_ERROR (FdtGetMmio (Fdt, Node, 0, SIZE_4KB, &Nor->Base, NULL)) &&
        Nor->Base == SG2042_FIRMWARE_SPI) { break; }
  }
  if (Node < 0 || FdtStringListSearch (Fdt, Node, "status", "reserved") < 0) { return EFI_NOT_FOUND; }
  Node = FdtNodeOffsetByCompatible (Fdt, -1, "sophgo,sg2042-clkgen");
  Status = FdtGetMmio (Fdt, Node, 0, sizeof (UINT32), &Nor->Gate, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (Nor->Gate, EFI_PAGE_SIZE);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (Nor->Base, EFI_PAGE_SIZE);
  if (EFI_ERROR (Status)) { return Status; }
  MmioOr32 (CpuMmioAddress (Nor->Gate), SG2042_SPIFMC_GATE);
  return Sg2042SpiReadId (Nor, Id);
}

VOID
PlatformFirmwareUpdateGetInfo (PLATFORM_FIRMWARE_UPDATE_INFO *Info)
{
  Info->ImageSize = SIZE_16MB;
  Info->PreservationNotice = L"The update writes the SCP firmware, coreboot and partition table on SPI1. It preserves SPI1 variables. Re-enter settings previously stored on SPI0.";
}

EFI_STATUS
PlatformFirmwareUpdateValidate (CONST VOID *Image, UINTN Size)
{
  SG2042_SPI_NOR Nor;
  EFI_STATUS Status = ValidateImage (Image, Size);
  return EFI_ERROR (Status) ? Status : GetBootFlash (&Nor);
}

EFI_STATUS
PlatformFirmwareUpdate (
  CONST VOID *Buffer, UINTN Size, FIRMWARE_UPDATE_PROGRESS Progress, VOID *Context
  )
{
  CONST UINT8 *Image = Buffer;
  SG2042_SPI_NOR Nor;
  EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *Fvb = NULL;
  EFI_FVB_ATTRIBUTES_2 Attributes;
  EFI_STATUS Status = ValidateImage (Image, Size);
  if (EFI_ERROR (Status)) { return Status; }
  Status = GetBootFlash (&Nor);
  if (EFI_ERROR (Status)) { return Status; }

  UINT32 FipSize = ReadUnaligned32 ((CONST VOID *)(Image + SG2042_SCP_TABLE + 40));
  // Commit the SCP partition table after both boot files have been verified.
  CONST struct { UINT32 Base; UINT32 Bytes; } Regions[] = {
    { ZSBL_OFFSET, SIZE_8MB }, { FIP_OFFSET, FipSize }, { SG2042_SCP_TABLE, SIZE_4KB }
  };
  UINTN Total = SIZE_8MB + FipSize + SIZE_4KB, Done = 0;
  UINT8 *Scratch = AllocatePool (2 * SIZE_4KB);
  if (!Scratch) { return EFI_OUT_OF_RESOURCES; }
  UINT8 *Expected = Scratch + SIZE_4KB;
  Status = FirmwareUpdateFreezeNv (&Fvb, &Attributes);
  if (EFI_ERROR (Status)) { FreePool (Scratch); return Status; }
  for (UINTN I = 0; I < ARRAY_SIZE (Regions) && !EFI_ERROR (Status); I++) {
    for (UINT32 Pos = 0; Pos < Regions[I].Bytes; Pos += SIZE_4KB) {
      UINT32 Offset = Regions[I].Base + Pos;
      UINTN Bytes = MIN (SIZE_4KB, Regions[I].Bytes - Pos);
      Status = Progress ? Progress (FirmwareUpdateRead, Done, Total, Context) : EFI_SUCCESS;
      if (EFI_ERROR (Status)) { break; }
      EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
      Status = Sg2042SpiRead (&Nor, Offset, SIZE_4KB, Scratch);
      gBS->RestoreTPL (OldTpl);
      if (EFI_ERROR (Status)) { break; }
      if (CompareMem (Scratch, Image + Offset, Bytes) != 0) {
        CopyMem (Expected, Scratch, SIZE_4KB);
        CopyMem (Expected, Image + Offset, Bytes);
        Status = Progress ? Progress (FirmwareUpdateErase, Done, Total, Context) : EFI_SUCCESS;
        if (EFI_ERROR (Status)) { break; }
        OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
        Status = Sg2042SpiErase (&Nor, Offset);
        gBS->RestoreTPL (OldTpl);
        if (EFI_ERROR (Status)) { break; }
        for (UINTN Page = 0; Page < SIZE_4KB; Page += 256) {
          UINTN Byte;
          for (Byte = 0; Byte < 256; Byte++) {
            if (Expected[Page + Byte] != 0xff) { break; }
          }
          if (Byte == 256) { continue; }
          OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
          Status = Sg2042SpiProgram (&Nor, Offset + Page, 256, Expected + Page);
          gBS->RestoreTPL (OldTpl);
          if (EFI_ERROR (Status)) { break; }
        }
        if (EFI_ERROR (Status)) { break; }
        Status = Progress ? Progress (FirmwareUpdateVerify, Done, Total, Context) : EFI_SUCCESS;
        if (EFI_ERROR (Status)) { break; }
        OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
        Status = Sg2042SpiRead (&Nor, Offset, SIZE_4KB, Scratch);
        gBS->RestoreTPL (OldTpl);
        if (!EFI_ERROR (Status) && CompareMem (Expected, Scratch, SIZE_4KB)) { Status = EFI_CRC_ERROR; }
        if (EFI_ERROR (Status)) { break; }
      }
      Done += Bytes;
    }
  }
  if (!EFI_ERROR (Status) && Progress) { Status = Progress (FirmwareUpdateVerify, Total, Total, Context); }
  if (Fvb != NULL) {
    EFI_STATUS Restore = Fvb->SetAttributes (Fvb, &Attributes);
    if (!EFI_ERROR (Status)) { Status = Restore; }
  }
  FreePool (Scratch);
  return Status;
}
