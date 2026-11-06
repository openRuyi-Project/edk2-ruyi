## SPDX-License-Identifier: BSD-2-Clause-Patent
[Defines]
  PLATFORM_NAME = SiFiveUnmatched
  PLATFORM_GUID = 94390b22-a436-48c2-86de-c935ee2fb49b
  PLATFORM_VERSION = 0.1
  DSC_SPECIFICATION = 0x0001001C
  OUTPUT_DIRECTORY = Build/SiFiveUnmatched
  SUPPORTED_ARCHITECTURES = RISCV64
  BUILD_TARGETS = DEBUG|RELEASE
  SKUID_IDENTIFIER = DEFAULT
  FLASH_DEFINITION = SiFivePlatformPkg/Unmatched/Unmatched.fdf
  DEFINE X64EMU_ENABLE = TRUE
  DEFINE MAU_DEFAULT_ENABLE = FALSE
  DEFINE RISCV_PEI_BOOTING = FALSE
  DEFINE SECURE_BOOT_ENABLE = FALSE
  DEFINE DEBUG_ON_SERIAL_PORT = TRUE
  # RiscV.dsc.inc has a legacy HTTP library path; use NetworkPkg below.
  DEFINE HTTP_BOOT_ENABLE = FALSE
  DEFINE NETWORK_PXE_BOOT_ENABLE = FALSE
  DEFINE NETWORK_HTTP_BOOT_ENABLE = FALSE
!if ($(NETWORK_PXE_BOOT_ENABLE) == TRUE) OR ($(NETWORK_HTTP_BOOT_ENABLE) == TRUE)
  DEFINE NETWORK_ENABLE = TRUE
!else
  DEFINE NETWORK_ENABLE = FALSE
!endif
  DEFINE NETWORK_SNP_ENABLE = FALSE
  DEFINE NETWORK_IP4_ENABLE = TRUE
  DEFINE NETWORK_IP6_ENABLE = TRUE
  DEFINE NETWORK_TLS_ENABLE = FALSE
  DEFINE NETWORK_ALLOW_HTTP_CONNECTIONS = TRUE
  DEFINE NETWORK_ISCSI_ENABLE = FALSE

!include Silicon/RISC-V/RiscV.dsc.inc

[PcdsFeatureFlag]
  # UEFI option ROMs can use the root bridge's 64-bit MMIO aperture.
  gEfiMdeModulePkgTokenSpaceGuid.PcdPciDegradeResourceForOptionRom|FALSE

[LibraryClasses]
  BootProgressLib|RuyiPlatformPkg/Library/BootProgressLib/BootProgressLib.inf
  BootManagerLib|RuyiPlatformPkg/Library/BootManagerLib/BootManagerLib.inf
  BootLogoLib|MdeModulePkg/Library/BootLogoLib/BootLogoLib.inf
  BmpSupportLib|MdeModulePkg/Library/BaseBmpSupportLib/BaseBmpSupportLib.inf
  FirmwareUpdateLib|RuyiPlatformPkg/Library/FirmwareUpdateLib/FirmwareUpdateLib.inf
  PlatformFirmwareUpdateLib|SiFivePlatformPkg/Library/UnmatchedFirmwareUpdateLib/UnmatchedFirmwareUpdateLib.inf
  Fu740SpiNorLib|SiFivePlatformPkg/Library/Fu740SpiNorLib/Fu740SpiNorLib.inf
  NvFvbLib|RuyiPlatformPkg/Library/NvFvbLib/NvFvbLib.inf
  BaseCryptLib|CryptoPkg/Library/BaseCryptLib/BaseCryptLib.inf
  OpensslLib|CryptoPkg/Library/OpensslLib/OpensslLib.inf
  IntrinsicLib|CryptoPkg/Library/IntrinsicLib/IntrinsicLib.inf
  PciGopPolicyLib|RuyiPlatformPkg/Library/PciGopPolicyLib/PciGopPolicyLib.inf
!if $(X64EMU_ENABLE) == TRUE
  PciGopLoaderLib|RuyiPlatformPkg/Library/PciGopLoaderLib/PciGopLoaderLib.inf
!else
  PciGopLoaderLib|RuyiPlatformPkg/Library/PciGopLoaderLibNull/PciGopLoaderLibNull.inf
!endif
  PlatformCpuInfoLib|SiFivePlatformPkg/Library/Fu740CpuInfoLib/Fu740CpuInfoLib.inf
  Fu740InfoLib|SiFivePlatformPkg/Library/Fu740InfoLib/Fu740InfoLib.inf
  PciSegmentLib|SiFivePlatformPkg/Library/Fu740PciSegmentLib/Fu740PciSegmentLib.inf
  PciHostBridgeLib|SiFivePlatformPkg/Library/Fu740PciHostBridgeLib/Fu740PciHostBridgeLib.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  UefiUsbLib|MdePkg/Library/UefiUsbLib/UefiUsbLib.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  OemHookStatusCodeLib|MdeModulePkg/Library/OemHookStatusCodeLibNull/OemHookStatusCodeLibNull.inf
  RealTimeClockLib|SiFivePlatformPkg/Library/Da9063RtcLib/Da9063RtcLib.inf
  ResetSystemLib|OvmfPkg/RiscVVirt/Library/ResetSystemLib/BaseResetSystemLib.inf
  RngLib|MdePkg/Library/BaseRngLibNull/BaseRngLibNull.inf

[PcdsFixedAtBuild]
  gEfiMdeModulePkgTokenSpaceGuid.PcdFrontPageLogoEnable|TRUE
  # Match RiscVVirt: avoid walking entire lists on every network/pool operation.
  gEfiMdePkgTokenSpaceGuid.PcdMaximumLinkedListLength|0
  gSiFivePlatformTokenSpaceGuid.PcdUnmatchedDefaultEmulation|$(MAU_DEFAULT_ENABLE)
  gSiFivePlatformTokenSpaceGuid.PcdUnmatchedEmulationAvailable|$(X64EMU_ENABLE)
  # FU740 has Sv39, but neither Svpbmt nor Zicbom.
  gEfiMdePkgTokenSpaceGuid.PcdRiscVFeatureOverride|0
  gUefiCpuPkgTokenSpaceGuid.PcdCpuRiscVMmuMaxSatpMode|8
  gUefiRiscVPlatformPkgTokenSpaceGuid.PcdRiscVPeiBootEnable|FALSE
  gUefiRiscVPlatformPkgTokenSpaceGuid.PcdForceNoAcpi|TRUE
  gUefiRiscVPlatformPkgTokenSpaceGuid.PcdTemporaryRamBase64|0x84000000
  gUefiRiscVPlatformPkgTokenSpaceGuid.PcdTemporaryRamSize|0x02000000
  gUefiCpuPkgTokenSpaceGuid.PcdPeiTemporaryRamStackSize|0x10000
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableSize|0x40000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingSize|0x10000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareSize|0x40000
  gEfiMdeModulePkgTokenSpaceGuid.PcdVariableStoreSize|0x40000
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxVariableSize|0x2000
  gEfiMdeModulePkgTokenSpaceGuid.PcdResetOnMemoryTypeInformationChange|FALSE
  gEfiMdePkgTokenSpaceGuid.PcdReportStatusCodePropertyMask|0x03
  gEfiMdeModulePkgTokenSpaceGuid.PcdStatusCodeUseSerial|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdStatusCodeUseMemory|FALSE
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2f
!if $(TARGET) == DEBUG
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000047
!else
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000002
!endif
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultBaudRate|115200
  gEfiMdePkgTokenSpaceGuid.PcdDefaultTerminalType|4
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootManagerMenuFile|{0x21,0xaa,0x2c,0x46,0x14,0x76,0x03,0x45,0x83,0x6e,0x8a,0xb6,0xf4,0x66,0x23,0x31}
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesCode|0x80
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesData|0x200
  gEfiMdeModulePkgTokenSpaceGuid.PcdSmbiosEntryPointProvideMethod|0x2

[PcdsDynamicDefault]
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVendor|L"openRuyi"
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString|L"EDK II stable202608"
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableBase64|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingBase64|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareBase64|0

[PcdsDynamicHii]
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|L"Timeout"|gEfiGlobalVariableGuid|0x0|5

[BuildOptions.common.EDKII.DXE_CORE, BuildOptions.common.EDKII.DXE_DRIVER, BuildOptions.common.EDKII.DXE_RUNTIME_DRIVER, BuildOptions.common.EDKII.UEFI_DRIVER, BuildOptions.common.EDKII.UEFI_APPLICATION]
  GCC:*_*_*_DLINK_FLAGS = -z common-page-size=0x1000

[Components]
  SiFivePlatformPkg/Drivers/UnmatchedSettingsDxe/UnmatchedSettingsDxe.inf
  SiFivePlatformPkg/Drivers/UnmatchedSettingsUiDxe/UnmatchedSettingsUiDxe.inf
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf {
    <LibraryClasses>
!if $(X64EMU_ENABLE) == TRUE
      NULL|RuyiPlatformPkg/Library/PciGopSecurityLib/PciGopSecurityLib.inf
!endif
  }
  SiFivePlatformPkg/Drivers/Fu740PciControllerDxe/Fu740PciControllerDxe.inf
  SiFivePlatformPkg/Drivers/PciCpuIo2Dxe/PciCpuIo2Dxe.inf
  MdeModulePkg/Bus/Pci/PciHostBridgeDxe/PciHostBridgeDxe.inf
  MdeModulePkg/Bus/Pci/PciBusDxe/PciBusDxe.inf
  MdeModulePkg/Bus/Pci/NvmExpressDxe/NvmExpressDxe.inf
  MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf
  MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf
  MdeModulePkg/Universal/Disk/RamDiskDxe/RamDiskDxe.inf

[Components]
  SiFivePlatformPkg/Drivers/UnmatchedSmbiosDxe/UnmatchedSmbiosDxe.inf
  RuyiPlatformPkg/Drivers/SystemInfoDxe/SystemInfoDxe.inf

[LibraryClasses.common.DXE_DRIVER]
  PlatformBootManagerLib|SiFivePlatformPkg/Library/BootManagerLib/BootManagerLib.inf

[LibraryClasses.common.DXE_RUNTIME_DRIVER]
  # SBI DBCN buffer writes require physical addresses, not OS runtime pointers.
  DebugLib|MdePkg/Library/DxeRuntimeDebugLibSerialPort/DxeRuntimeDebugLibSerialPort.inf

[Components]
  MdeModulePkg/Application/BootManagerMenuApp/BootManagerMenuApp.inf

[PcdsDynamicExDefault]
  gMauPkgTokenSpaceGuid.PcdMauEmulationEnabled|FALSE
!if $(X64EMU_ENABLE) == TRUE
[Components]
  RuyiPlatformPkg/Drivers/EmulatorDxe/EmulatorDxe.inf
!endif

[Components]
  SecurityPkg/Hash2DxeCrypto/Hash2DxeCrypto.inf

!if $(NETWORK_ENABLE) == TRUE
# FU740 has no hardware entropy source. The timer RNG supplies network IDs;
# HTTPS is disabled until a cryptographic entropy source is available.
[PcdsFixedAtBuild]
  gEfiMdePkgTokenSpaceGuid.PcdEnforceSecureRngAlgorithms|FALSE
[Components]
  SiFivePlatformPkg/Drivers/Fu740GemDxe/Fu740GemDxe.inf
  SiFivePlatformPkg/Drivers/NetworkRngDxe/NetworkRngDxe.inf {
    <LibraryClasses>
      RngLib|MdeModulePkg/Library/BaseRngLibTimerLib/BaseRngLibTimerLib.inf
  }
!endif
!include NetworkPkg/Network.dsc.inc

[Components]
  SiFivePlatformPkg/Drivers/UnmatchedNvStorageDxe/UnmatchedNvStorageDxe.inf
  RuyiPlatformPkg/Drivers/LogoDxe/LogoDxe.inf
  MdeModulePkg/Universal/Acpi/BootGraphicsResourceTableDxe/BootGraphicsResourceTableDxe.inf
  MdeModulePkg/Universal/FaultTolerantWriteDxe/FaultTolerantWriteDxe.inf {
    <LibraryClasses>
      NULL|SiFivePlatformPkg/Library/UnmatchedNvReadyLib/UnmatchedFtwReadyLib.inf
  }
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf {
    <LibraryClasses>
      NULL|SiFivePlatformPkg/Library/UnmatchedNvReadyLib/UnmatchedNvReadyLib.inf
  }

[Components]
  MdeModulePkg/Application/UiApp/UiApp.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/DeviceManagerUiLib/DeviceManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootManagerUiLib/BootManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootMaintenanceManagerUiLib/BootMaintenanceManagerUiLib.inf
      ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
      NULL|RuyiPlatformPkg/Library/FirmwareUpdateUiLib/FirmwareUpdateUiLib.inf
      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
  }
