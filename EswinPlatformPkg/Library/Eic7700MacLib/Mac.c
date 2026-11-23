/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/Eic7700BoardProfileLib.h>
#include <Library/Eic7700SpiNorLib.h>
#include <Library/FdtBoardInfoLib.h>
#include <Library/FdtLib.h>
#include <Library/PlatformMacAddressLib.h>
#include <Library/PrintLib.h>
#include "Internal.h"

#define BOARD_INFO_STRIDE 0x40000U
#define BOARD_INFO_BYTES  0x1000U
#define BOARD_INFO_MAGIC  0x05454943U
#define BOARD_INFO_MAC    36U

STATIC_ASSERT (BOARD_INFO_MAC + sizeof (EIC7700_MAC_PAIR) <= BOARD_INFO_BYTES - sizeof (UINT32),
               "Factory MAC addresses overlap the checksum");

STATIC EIC7700_MAC_PAIR mMac;
STATIC BOOLEAN mPairReady;

STATIC BOOLEAN
ValidMac (CONST UINT8 *Mac)
{
  return !(Mac[0] & BIT0) && !IsZeroBuffer (Mac, PXE_HWADDR_LEN_ETHER);
}

BOOLEAN
Eic7700MacPairValid (CONST EIC7700_MAC_PAIR *Pair)
{
  return ValidMac (Pair->Address[0]) && ValidMac (Pair->Address[1]) &&
         CompareMem (Pair->Address[0], Pair->Address[1], sizeof (Pair->Address[0])) != 0;
}

STATIC BOOLEAN
DecodeRecord (CONST UINT8 *Data, EIC7700_MAC_PAIR *Pair)
{
  UINT32 Crc = 0;
  if (ReadUnaligned32 ((CONST UINT32 *)Data) != BOARD_INFO_MAGIC || Data[6] != 4) {
    return FALSE;
  }
  // The vendor record uses zlib CRC32 with an initial argument of 0xffffffff.
  for (UINTN I = 0; I < BOARD_INFO_BYTES - sizeof (Crc); I++) {
    Crc ^= Data[I];
    for (UINTN Bit = 0; Bit < 8; Bit++) {
      Crc = (Crc >> 1) ^ ((Crc & 1) ? 0xedb88320U : 0);
    }
  }
  if (~Crc != ReadUnaligned32 ((CONST UINT32 *)(Data + BOARD_INFO_BYTES - sizeof (Crc)))) {
    return FALSE;
  }
  CopyMem (Pair, Data + BOARD_INFO_MAC, sizeof (*Pair));
  return Eic7700MacPairValid (Pair);
}

STATIC EFI_STATUS
FromDeviceTree (UINTN Index, UINT8 *Mac)
{
  RUYI_BOARD_INFO Info;
  CONST VOID *Fdt;
  CONST CHAR8 *Path;
  CONST UINT8 *Data;
  CHAR8 Name[16];
  INT32 Aliases, Node, Length;
  EFI_STATUS Status = FdtBoardInfoGet (&Info, &Fdt);
  if (EFI_ERROR (Status)) { return Status; }
  Aliases = FdtPathOffset (Fdt, "/aliases");
  if (Aliases < 0) { return EFI_NOT_FOUND; }
  AsciiSPrint (Name, sizeof (Name), "ethernet%u", Index);
  Path = FdtGetProp (Fdt, Aliases, Name, &Length);
  if (!Path || Length < 2 || Path[Length - 1]) { return EFI_NOT_FOUND; }
  Node = FdtPathOffset (Fdt, Path);
  if (Node < 0) { return EFI_NOT_FOUND; }
  Data = FdtGetProp (Fdt, Node, "mac-address", &Length);
  if (!Data || Length != PXE_HWADDR_LEN_ETHER || !ValidMac (Data)) {
    Data = FdtGetProp (Fdt, Node, "local-mac-address", &Length);
  }
  if (!Data || Length != PXE_HWADDR_LEN_ETHER || !ValidMac (Data)) { return EFI_NOT_FOUND; }
  CopyMem (Mac, Data, PXE_HWADDR_LEN_ETHER);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
ReadPair (VOID)
{
  EIC7700_MAC_PAIR Candidate;
  UINT8 Record[BOARD_INFO_BYTES];
  BOOLEAN Found = FALSE;
  UINT16 Generation = 0;
  EFI_STATUS Status;

  CONST EIC7700_BOARD_PROFILE *Board = Eic7700GetBoardProfile ();
  if (!Board) { return EFI_UNSUPPORTED; }
  // P550 stores its factory addresses in the carrier EEPROM.
  if (!Board->FactoryBytes) { return Eic7700ReadEepromMacPair (&mMac); }
  Status = FromDeviceTree (0, mMac.Address[0]);
  if (!EFI_ERROR (Status)) {
    Status = FromDeviceTree (1, mMac.Address[1]);
    if (!EFI_ERROR (Status) && Eic7700MacPairValid (&mMac)) {
      return EFI_SUCCESS;
    }
  }
  if (EFI_ERROR (Status) && Status != EFI_NOT_FOUND) { return Status; }
  Status = Eic7700SpiNorInit (FALSE);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN Slot = 0; Slot < 2; Slot++) {
    UINT32 Offset = Board->FactoryOffset + Slot * BOARD_INFO_STRIDE;
    Status = Eic7700SpiNorRead (Offset, sizeof (Record), Record);
    if (EFI_ERROR (Status)) { return Status; }
    if (!DecodeRecord (Record, &Candidate)) { continue; }
    UINT16 Next = ReadUnaligned16 ((CONST UINT16 *)(Record + 4));
    if (!Found || (INT16)(Next - Generation) > 0) {
      mMac = Candidate;
      Generation = Next;
      Found = TRUE;
    }
  }
  return Found ? EFI_SUCCESS : Eic7700GenerateMacPair (&mMac);
}

EFI_STATUS
PlatformGetMacAddress (UINTN Index, UINT8 Mac[PXE_HWADDR_LEN_ETHER])
{
  EFI_STATUS Status;

  if (!Mac || Index >= ARRAY_SIZE (mMac.Address)) { return EFI_INVALID_PARAMETER; }
  if (!mPairReady) {
    Status = ReadPair ();
    if (EFI_ERROR (Status)) { return Status; }
    mPairReady = TRUE;
  }
  CopyMem (Mac, mMac.Address[Index], sizeof (mMac.Address[Index]));
  return EFI_SUCCESS;
}
