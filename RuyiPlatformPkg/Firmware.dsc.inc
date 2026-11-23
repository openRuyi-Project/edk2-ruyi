## SPDX-License-Identifier: BSD-2-Clause-Patent
[Defines]
!ifndef $(FIRMWARE_VER)
  DEFINE FIRMWARE_VER = EDK II
!endif
!ifndef $(FIRMWARE_VENDOR)
  DEFINE FIRMWARE_VENDOR = openRuyi
!endif

[PcdsFixedAtBuild]
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVendor|L"$(FIRMWARE_VENDOR)"
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString|L"$(FIRMWARE_VER)"

[LibraryClasses]
  RuyiSmbiosLib|RuyiPlatformPkg/Library/RuyiSmbiosLib/RuyiSmbiosLib.inf
