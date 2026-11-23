/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/RuyiSmbiosLib.h>

EFI_STATUS
EFIAPI
RuyiSmbiosAddRecord (
  IN EFI_SMBIOS_PROTOCOL  *Smbios,
  IN SMBIOS_STRUCTURE     *Header,
  IN CONST CHAR8          **Strings,
  IN UINTN                StringCount,
  OUT EFI_SMBIOS_HANDLE   *Handle
  )
{
  SMBIOS_STRUCTURE  *Record;
  CHAR8             *Next;
  UINTN             Size, Index, Length;
  EFI_STATUS        Status;

  Size = Header->Length + 2;
  for (Index = 0; Index < StringCount; Index++) {
    Size += AsciiStrSize (Strings[Index]);
  }

  Record = AllocateZeroPool (Size);
  if (Record == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Record, Header, Header->Length);
  Next = (CHAR8 *)Record + Header->Length;
  for (Index = 0; Index < StringCount; Index++) {
    Length = AsciiStrSize (Strings[Index]);
    CopyMem (Next, Strings[Index], Length);
    Next += Length;
  }

  *Handle = SMBIOS_HANDLE_PI_RESERVED;
  Status  = Smbios->Add (Smbios, NULL, Handle, Record);
  FreePool (Record);
  return Status;
}

EFI_STATUS
EFIAPI
RuyiSmbiosAddBiosRecord (
  IN EFI_SMBIOS_PROTOCOL  *Smbios,
  IN SMBIOS_TABLE_TYPE0   *Bios,
  OUT EFI_SMBIOS_HANDLE   *Handle
  )
{
  CHAR8          Vendor[FixedPcdGetSize (PcdFirmwareVendor) / sizeof (CHAR16)];
  CHAR8          Version[FixedPcdGetSize (PcdFirmwareVersionString) / sizeof (CHAR16)];
  CONST CHAR8    *Strings[] = { Vendor, Version };
  RETURN_STATUS  Status;

  Status = UnicodeStrToAsciiStrS (FixedPcdGetPtr (PcdFirmwareVendor), Vendor, sizeof (Vendor));
  if (RETURN_ERROR (Status)) {
    return Status;
  }

  Status = UnicodeStrToAsciiStrS (FixedPcdGetPtr (PcdFirmwareVersionString), Version, sizeof (Version));
  if (RETURN_ERROR (Status)) {
    return Status;
  }

  Bios->Vendor = 1;
  Bios->BiosVersion = 2;
  return RuyiSmbiosAddRecord (Smbios, &Bios->Hdr, Strings, ARRAY_SIZE (Strings), Handle);
}
