## SPDX-License-Identifier: BSD-2-Clause-Patent
[PcdsFixedAtBuild]
  gEfiMdePkgTokenSpaceGuid.PcdEnforceSecureRngAlgorithms|FALSE

[Components]
  MdeModulePkg/Universal/Disk/RamDiskDxe/RamDiskDxe.inf
  RuyiPlatformPkg/Drivers/NetworkRngDxe/NetworkRngDxe.inf {
    <LibraryClasses>
      RngLib|MdeModulePkg/Library/BaseRngLibTimerLib/BaseRngLibTimerLib.inf
  }

!include NetworkPkg/Network.dsc.inc
