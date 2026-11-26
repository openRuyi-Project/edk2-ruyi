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
#define BOARD_INFO_DIE_BYTES  0x600U

STATIC_ASSERT (BOARD_INFO_MAC + sizeof (EIC7700_MAC_PAIR) <= BOARD_INFO_BYTES - sizeof (UINT32),
               "Factory MAC addresses overlap the checksum");

STATIC_ASSERT (BOARD_INFO_MAC + BOARD_INFO_DIE_BYTES + sizeof (EIC7700_MAC_PAIR) <=
               BOARD_INFO_BYTES - sizeof (UINT32), "Die 1 MAC addresses overlap the checksum");

STATIC EIC7700_MAC mMac[D560_ETHERNET_PORTS];
STATIC UINTN mPortCount;

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
ValidRecord (CONST UINT8 *Data)
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
  return TRUE;
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
ReadFactory (UINT32 Offset, EIC7700_MAC_PAIR *Die0, EIC7700_MAC_PAIR *Die1 OPTIONAL)
{
  UINT8 Record[BOARD_INFO_BYTES];
  BOOLEAN Found = FALSE;
  UINT16 Generation = 0;
  EFI_STATUS Status;

  Status = Eic7700SpiNorInit (FALSE);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN Slot = 0; Slot < 2; Slot++) {
    Status = Eic7700SpiNorRead (Offset + Slot * BOARD_INFO_STRIDE, sizeof (Record), Record);
    if (EFI_ERROR (Status)) { return Status; }
    if (!ValidRecord (Record)) { continue; }
    EIC7700_MAC_PAIR Candidate;
    CopyMem (&Candidate, Record + BOARD_INFO_MAC, sizeof (Candidate));
    if (Die1 == NULL && !Eic7700MacPairValid (&Candidate)) { continue; }
    UINT16 Next = ReadUnaligned16 ((CONST UINT16 *)(Record + 4));
    if (!Found || (INT16)(Next - Generation) > 0) {
      *Die0 = Candidate;
      if (Die1 != NULL) {
        CopyMem (Die1, Record + BOARD_INFO_MAC + BOARD_INFO_DIE_BYTES, sizeof (*Die1));
      }
      Generation = Next;
      Found = TRUE;
    }
  }
  return Found ? EFI_SUCCESS : EFI_NOT_FOUND;
}

STATIC EFI_STATUS
ReadPair (CONST EIC7700_BOARD_PROFILE *Board, EIC7700_MAC_PAIR *Pair)
{
  EFI_STATUS Status;

  // P550 stores its factory addresses in the carrier EEPROM.
  if (!Board->FactoryBytes) { return Eic7700ReadEepromMacPair (Pair); }
  Status = FromDeviceTree (0, Pair->Address[0]);
  if (!EFI_ERROR (Status)) {
    Status = FromDeviceTree (1, Pair->Address[1]);
    if (!EFI_ERROR (Status) && Eic7700MacPairValid (Pair)) {
      return EFI_SUCCESS;
    }
  }
  if (EFI_ERROR (Status) && Status != EFI_NOT_FOUND) { return Status; }
  Status = ReadFactory (Board->FactoryOffset, Pair, NULL);
  return Status == EFI_NOT_FOUND ? Eic7700GenerateMacPair (Pair) : Status;
}

STATIC UINTN
MergeD560Macs (EIC7700_MAC *Macs, CONST EIC7700_MAC *Candidates)
{
  UINTN Missing = 0;
  for (UINTN Port = 0; Port < D560_ETHERNET_PORTS; Port++) {
    if (ValidMac (Macs[Port])) { continue; }
    if (ValidMac (Candidates[Port]) && !Eic7700MacInUse (Macs, D560_ETHERNET_PORTS, Candidates[Port])) {
      CopyMem (Macs[Port], Candidates[Port], sizeof (EIC7700_MAC));
    } else {
      Missing++;
    }
  }
  return Missing;
}

STATIC EFI_STATUS
ReadD560Macs (CONST EIC7700_BOARD_PROFILE *Board, EIC7700_MAC *Macs)
{
  EIC7700_MAC Candidates[D560_ETHERNET_PORTS] = { 0 };
  EIC7700_MAC_PAIR Factory[2];
  EFI_STATUS Status;

  for (UINTN Port = 0; Port < D560_ETHERNET_PORTS; Port++) {
    Status = FromDeviceTree (Port, Candidates[Port]);
    if (EFI_ERROR (Status) && Status != EFI_NOT_FOUND) { return Status; }
  }
  if (MergeD560Macs (Macs, Candidates) == 0) { return EFI_SUCCESS; }

  // Fill missing ports from the saved environment before the factory record.
  Status = Eic7700SpiNorInit (FALSE);
  if (EFI_ERROR (Status)) { return Status; }
  ZeroMem (Candidates, sizeof (Candidates));
  Status = D560ReadEnvironmentMacs (Candidates);
  if (EFI_ERROR (Status)) { return Status; }
  if (MergeD560Macs (Macs, Candidates) == 0) { return EFI_SUCCESS; }

  Status = ReadFactory (Board->FactoryOffset, &Factory[0], &Factory[1]);
  if (!EFI_ERROR (Status)) {
    CopyMem (Candidates, Factory, sizeof (Candidates));
    if (MergeD560Macs (Macs, Candidates) == 0) { return EFI_SUCCESS; }
  } else if (Status != EFI_NOT_FOUND) {
    return Status;
  }
  return Eic7700FillLocalMacs (Macs, D560_ETHERNET_PORTS);
}

EFI_STATUS
PlatformGetMacAddress (UINTN Index, UINT8 Mac[PXE_HWADDR_LEN_ETHER])
{
  EFI_STATUS Status;

  if (Mac == NULL || Index >= ARRAY_SIZE (mMac)) { return EFI_INVALID_PARAMETER; }
  if (mPortCount == 0) {
    CONST EIC7700_BOARD_PROFILE *Board = Eic7700GetBoardProfile ();
    if (Board == NULL) { return EFI_UNSUPPORTED; }
    if (Index >= Board->EthernetPorts) { return EFI_INVALID_PARAMETER; }
    if (Board->EthernetPorts == D560_ETHERNET_PORTS) {
      ZeroMem (mMac, sizeof (mMac));
      Status = ReadD560Macs (Board, mMac);
    } else {
      EIC7700_MAC_PAIR Pair;
      Status = ReadPair (Board, &Pair);
      if (!EFI_ERROR (Status)) { CopyMem (mMac, &Pair, sizeof (Pair)); }
    }
    if (EFI_ERROR (Status)) { return Status; }
    mPortCount = Board->EthernetPorts;
  }
  if (Index >= mPortCount) { return EFI_INVALID_PARAMETER; }
  CopyMem (Mac, mMac[Index], sizeof (mMac[Index]));
  return EFI_SUCCESS;
}
