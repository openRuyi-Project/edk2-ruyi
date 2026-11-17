/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/Eic7700SpiNorLib.h>
#include <Library/FdtBoardInfoLib.h>
#include <Library/FdtLib.h>
#include <Library/PlatformMacAddressLib.h>
#include <Library/PrintLib.h>

#define BOARD_INFO_BASE   0x780000U
#define BOARD_INFO_STRIDE 0x40000U
#define BOARD_INFO_BYTES  0x1000U
#define BOARD_INFO_MAGIC  0x05454943U
#define BOARD_INFO_MAC    36U

STATIC UINT8 mMac[2][6];
STATIC BOOLEAN mValid[2];

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

EFI_STATUS
PlatformGetMacAddress (UINTN Index, UINT8 Mac[6])
{
  UINT8 Records[2][BOARD_INFO_BYTES], Seed[9], Hash[SHA256_DIGEST_SIZE];
  BOOLEAN Valid[2];
  UINTN Selected;
  EFI_STATUS Status;
  if (!Mac || Index >= ARRAY_SIZE (mMac)) { return EFI_INVALID_PARAMETER; }
  if (mValid[Index]) { CopyMem (Mac, mMac[Index], 6); return EFI_SUCCESS; }
  Status = FromDeviceTree (Index, mMac[Index]);
  if (!EFI_ERROR (Status)) { goto Done; }
  if (Status != EFI_NOT_FOUND) { return Status; }
  Status = Eic7700SpiNorInit (FALSE);
  if (EFI_ERROR (Status)) { return Status; }
  for (UINTN I = 0; I < 2; I++) {
    Status = Eic7700SpiNorRead (BOARD_INFO_BASE + I * BOARD_INFO_STRIDE,
                              BOARD_INFO_BYTES, Records[I]);
    if (EFI_ERROR (Status)) { return Status; }
    Valid[I] = ValidRecord (Records[I]);
  }
  if (Valid[0] || Valid[1]) {
    Selected = !Valid[0] ? 1 : 0;
    if (Valid[0] && Valid[1] &&
        (INT16)(ReadUnaligned16 ((UINT16 *)(Records[1] + 4)) -
                ReadUnaligned16 ((UINT16 *)(Records[0] + 4))) > 0) { Selected = 1; }
    CopyMem (mMac[Index], Records[Selected] + BOARD_INFO_MAC + Index * 6, 6);
  } else {
    Status = Eic7700SpiNorReadUniqueId (Seed);
    if (EFI_ERROR (Status)) { return Status; }
    Seed[8] = (UINT8)Index;
    if (!Sha256HashAll (Seed, sizeof (Seed), Hash)) { return EFI_DEVICE_ERROR; }
    // A stable local address is used only when no valid board address exists.
    mMac[Index][0] = 0x02;
    CopyMem (mMac[Index] + 1, Hash, 5);
  }
Done:
  mValid[Index] = TRUE;
  CopyMem (Mac, mMac[Index], 6);
  return EFI_SUCCESS;
}
