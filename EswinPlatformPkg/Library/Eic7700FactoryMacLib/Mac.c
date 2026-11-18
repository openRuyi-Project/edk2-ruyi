/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/Eic7700BoardProfileLib.h>
#include <Library/Eic7700SpiNorLib.h>
#include <Library/FdtBoardInfoLib.h>
#include <Library/FdtLib.h>
#include <Library/PlatformMacAddressLib.h>
#include <Library/PrintLib.h>

#define BOARD_INFO_STRIDE 0x40000U
#define BOARD_INFO_BYTES  0x1000U
#define BOARD_INFO_MAGIC  0x05454943U
#define BOARD_INFO_MAC    36U

STATIC UINT8 mMac[2][6];
STATIC BOOLEAN mPairReady;

STATIC BOOLEAN
ValidMac (CONST UINT8 Mac[6])
{
  UINT8 Any = 0;
  for (UINTN I = 0; I < 6; I++) { Any |= Mac[I]; }
  return Any != 0 && !(Mac[0] & 1);
}

STATIC BOOLEAN
ValidRecord (CONST UINT8 *Data)
{
  UINT32 Crc = 0;
  if (ReadUnaligned32 ((CONST UINT32 *)Data) != BOARD_INFO_MAGIC || Data[6] != 4) {
    return FALSE;
  }
  // The vendor record uses zlib CRC32 with an initial argument of 0xffffffff.
  for (UINTN I = 0; I < BOARD_INFO_BYTES - 4; I++) {
    Crc ^= Data[I];
    for (UINTN Bit = 0; Bit < 8; Bit++) {
      Crc = (Crc >> 1) ^ ((Crc & 1) ? 0xedb88320U : 0);
    }
  }
  return ~Crc == ReadUnaligned32 ((CONST UINT32 *)(Data + BOARD_INFO_BYTES - 4)) &&
         ValidMac (Data + BOARD_INFO_MAC) && ValidMac (Data + BOARD_INFO_MAC + 6) &&
         CompareMem (Data + BOARD_INFO_MAC, Data + BOARD_INFO_MAC + 6, 6) != 0;
}

STATIC EFI_STATUS
FromDeviceTree (UINTN Index, UINT8 Mac[6])
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
  if (!Data || Length != 6 || !ValidMac (Data)) {
    Data = FdtGetProp (Fdt, Node, "local-mac-address", &Length);
  }
  if (!Data || Length != 6 || !ValidMac (Data)) { return EFI_NOT_FOUND; }
  CopyMem (Mac, Data, 6);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
GeneratePair (VOID)
{
  UINT8 Seed[EIC7700_SPI_NOR_MAX_UID_SIZE + 1], Hash[SHA256_DIGEST_SIZE];
  UINTN Size = sizeof (Seed) - 1;
  EFI_STATUS Status;

  Status = Eic7700SpiNorReadUniqueId (Seed, &Size);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN Port = 0; Port < ARRAY_SIZE (mMac); Port++) {
    Seed[Size] = (UINT8)Port;
    if (!Sha256HashAll (Seed, Size + 1, Hash)) { return EFI_DEVICE_ERROR; }
    mMac[Port][0] = 0x02;
    CopyMem (mMac[Port] + 1, Hash, 5);
  }
  if (CompareMem (mMac[0], mMac[1], 6) == 0) {
    mMac[1][5] ^= 1;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
ReadPair (VOID)
{
  UINT8 Record[BOARD_INFO_BYTES];
  BOOLEAN Found = FALSE;
  UINT16 Generation = 0;
  EFI_STATUS Status;

  Status = FromDeviceTree (0, mMac[0]);
  if (!EFI_ERROR (Status)) {
    Status = FromDeviceTree (1, mMac[1]);
    if (!EFI_ERROR (Status) && CompareMem (mMac[0], mMac[1], 6) != 0) {
      return EFI_SUCCESS;
    }
  }
  if (EFI_ERROR (Status) && Status != EFI_NOT_FOUND) { return Status; }
  Status = Eic7700SpiNorInit (FALSE);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN Slot = 0; Slot < 2; Slot++) {
    UINT32 Offset = Eic7700GetBoardProfile ()->FactoryOffset + Slot * BOARD_INFO_STRIDE;
    Status = Eic7700SpiNorRead (Offset, sizeof (Record), Record);
    if (EFI_ERROR (Status)) { return Status; }
    if (!ValidRecord (Record)) { continue; }
    UINT16 Next = ReadUnaligned16 ((CONST UINT16 *)(Record + 4));
    if (!Found || (INT16)(Next - Generation) > 0) {
      CopyMem (mMac, Record + BOARD_INFO_MAC, sizeof (mMac));
      Generation = Next;
      Found = TRUE;
    }
  }
  return Found ? EFI_SUCCESS : GeneratePair ();
}

EFI_STATUS
PlatformGetMacAddress (UINTN Index, UINT8 Mac[6])
{
  EFI_STATUS Status;

  if (!Mac || Index >= ARRAY_SIZE (mMac)) { return EFI_INVALID_PARAMETER; }
  if (!mPairReady) {
    Status = ReadPair ();
    if (EFI_ERROR (Status)) { return Status; }
    mPairReady = TRUE;
  }
  CopyMem (Mac, mMac[Index], 6);
  return EFI_SUCCESS;
}
