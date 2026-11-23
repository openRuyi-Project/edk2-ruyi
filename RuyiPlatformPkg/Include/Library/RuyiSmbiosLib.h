/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once

#include <Uefi.h>
#include <Protocol/Smbios.h>

EFI_STATUS
EFIAPI
RuyiSmbiosAddRecord (
  IN EFI_SMBIOS_PROTOCOL  *Smbios,
  IN SMBIOS_STRUCTURE     *Header,
  IN CONST CHAR8          **Strings,
  IN UINTN                StringCount,
  OUT EFI_SMBIOS_HANDLE   *Handle
  );

EFI_STATUS
EFIAPI
RuyiSmbiosAddBiosRecord (
  IN EFI_SMBIOS_PROTOCOL  *Smbios,
  IN SMBIOS_TABLE_TYPE0   *Bios,
  OUT EFI_SMBIOS_HANDLE   *Handle
  );
