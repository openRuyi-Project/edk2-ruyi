/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/FastbootStorageLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
EFI_STATUS FastbootGetCommonVariable (CONST CHAR8 *Name, CHAR8 *Value, UINTN Size,
                                     FASTBOOT_TARGET_CAPACITY Capacity)
{
  CONST CHAR8 *Partition;
  UINT64 Bytes;
  CHAR8 Text[19];
  EFI_STATUS Status;
  if (Name == NULL || Value == NULL || Size == 0 || Capacity == NULL) { return EFI_INVALID_PARAMETER; }
  if (!AsciiStrCmp (Name, "version-bootloader")) {
    return UnicodeStrToAsciiStrS (PcdGetPtr (PcdFirmwareVersionString), Value, Size);
  }
  if (!AsciiStrnCmp (Name, "partition-size:", 15) || !AsciiStrnCmp (Name, "partition-type:", 15)) {
    Partition = Name + 15;
  } else if (!AsciiStrnCmp (Name, "has-slot:", 9)) { Partition = Name + 9; }
  else { return EFI_NOT_FOUND; }
  Status = Capacity (Partition, &Bytes);
  if (EFI_ERROR (Status)) { return Status; }
  if (!AsciiStrnCmp (Name, "partition-size:", 15)) {
    AsciiSPrint (Text, sizeof (Text), "0x%Lx", Bytes);
    return AsciiStrCpyS (Value, Size, Text);
  }
  return AsciiStrCpyS (Value, Size, !AsciiStrnCmp (Name, "has-slot:", 9) ? "no" : "raw");
}
