## SPDX-License-Identifier: BSD-2-Clause-Patent
[Defines]
  PLATFORM_NAME = SG2042
  PLATFORM_GUID = c10e1ccf-8972-4bab-ad6d-0c9a7b76b9e4
  PLATFORM_VERSION = 0.1
  DSC_SPECIFICATION = 0x0001001C
  OUTPUT_DIRECTORY = Build/SG2042
  SUPPORTED_ARCHITECTURES = RISCV64
  BUILD_TARGETS = DEBUG|RELEASE
  SKUID_IDENTIFIER = DEFAULT
  FLASH_DEFINITION = SophgoPlatformPkg/SG2042/SG2042.fdf
  DEFINE X64EMU_ENABLE = TRUE
  DEFINE RISCV_PEI_BOOTING = FALSE
  DEFINE SECURE_BOOT_ENABLE = FALSE
  DEFINE DEBUG_ON_SERIAL_PORT = TRUE
  # RiscV.dsc.inc has a legacy HTTP library path; use NetworkPkg below.
  DEFINE HTTP_BOOT_ENABLE = FALSE

!include RuyiPlatformPkg/NetworkDefines.dsc.inc
!include Silicon/RISC-V/RiscV.dsc.inc

[PcdsFeatureFlag]
  gEmbeddedTokenSpaceGuid.PcdRtcLibStoresTimeSettings|TRUE
  # UEFI option ROMs can use the root bridge's 64-bit MMIO aperture.
  gEfiMdeModulePkgTokenSpaceGuid.PcdPciDegradeResourceForOptionRom|FALSE

!include RuyiPlatformPkg/Debug.dsc.inc
!include RuyiPlatformPkg/Console.dsc.inc
!include RuyiPlatformPkg/Firmware.dsc.inc

[LibraryClasses]
  FirmwareUpdateLib|RuyiPlatformPkg/Library/FirmwareUpdateLib/FirmwareUpdateLib.inf
  PlatformFirmwareUpdateLib|SophgoPlatformPkg/Library/Sg2042FirmwareUpdateLib/Sg2042FirmwareUpdateLib.inf
  CorebootImageLib|RuyiPlatformPkg/Library/CorebootImageLib/CorebootImageLib.inf
  Sg2042SpiNorLib|SophgoPlatformPkg/Library/Sg2042SpiNorLib/Sg2042SpiNorLib.inf
  CpuMmioAddressLib|SophgoPlatformPkg/Library/Sg2042MmioAddressLib/Sg2042MmioAddressLib.inf
  Sg2042MacLib|SophgoPlatformPkg/Library/Sg2042MacLib/Sg2042MacLib.inf
  Sg2042ClockLib|SophgoPlatformPkg/Library/Sg2042ClockLib/Sg2042ClockLib.inf
  Rtl812xSnpLib|RuyiPlatformPkg/Library/Rtl812xSnpLib/Rtl812xSnpLib.inf
  Rtl812xPacketsLib|RuyiPlatformPkg/Library/Rtl812xPacketsLib/Rtl812xPacketsLib.inf
  Rtl812xHwLib|SophgoPlatformPkg/Library/Rtl8125HwLib/Rtl8125HwLib.inf
  SnpAdapterInfoLib|RuyiPlatformPkg/Library/SnpAdapterInfoLib/SnpAdapterInfoLib.inf
  PlatformBootPolicyLib|RuyiPlatformPkg/Library/DefaultBootPolicyLib/DefaultBootPolicyLib.inf
  CpuConfigLib|RuyiPlatformPkg/Library/CpuConfigLibNull/CpuConfigLibNull.inf
  MemoryConfigLib|RuyiPlatformPkg/Library/MemoryConfigLibNull/MemoryConfigLibNull.inf
  FdtBoardInfoLib|RuyiPlatformPkg/Library/FdtBoardInfoLib/FdtBoardInfoLib.inf
  NetworkBootConfigLib|RuyiPlatformPkg/Library/PlatformNetworkBootConfigLib/PlatformNetworkBootConfigLib.inf
  BootProgressLib|RuyiPlatformPkg/Library/BootProgressLib/BootProgressLib.inf
  BootManagerLib|RuyiPlatformPkg/Library/BootManagerLib/BootManagerLib.inf
  BootLogoLib|MdeModulePkg/Library/BootLogoLib/BootLogoLib.inf
  BmpSupportLib|MdeModulePkg/Library/BaseBmpSupportLib/BaseBmpSupportLib.inf
  FdtHardwareLib|RuyiPlatformPkg/Library/FdtHardwareLib/FdtHardwareLib.inf
  NvStorageLib|RuyiPlatformPkg/Library/NvStorageLib/NvStorageLib.inf
  DmaLib|EmbeddedPkg/Library/NonCoherentDmaLib/NonCoherentDmaLib.inf
  NonDiscoverableDeviceRegistrationLib|MdeModulePkg/Library/NonDiscoverableDeviceRegistrationLib/NonDiscoverableDeviceRegistrationLib.inf
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
  PlatformCpuInfoLib|SophgoPlatformPkg/Library/Sg2042CpuInfoLib/Sg2042CpuInfoLib.inf
  PciSegmentLib|RuyiPlatformPkg/Library/ControllerPciSegmentLib/ControllerPciSegmentLib.inf
  PciHostBridgeLib|RuyiPlatformPkg/Library/ControllerPciHostBridgeLib/ControllerPciHostBridgeLib.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  UefiUsbLib|MdePkg/Library/UefiUsbLib/UefiUsbLib.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  OemHookStatusCodeLib|MdeModulePkg/Library/OemHookStatusCodeLibNull/OemHookStatusCodeLibNull.inf
  RealTimeClockLib|RuyiPlatformPkg/Library/FdtVirtualRtcLib/FdtVirtualRtcLib.inf
  FdtRtcSourceLib|RuyiPlatformPkg/Library/FdtDs1307RtcSourceLib/FdtDs1307RtcSourceLib.inf
  DwI2cReadLib|RuyiPlatformPkg/Library/DwI2cReadLib/DwI2cReadLib.inf
  FdtI2cReadLib|SophgoPlatformPkg/Library/Sg2042I2cReadLib/Sg2042I2cReadLib.inf
  ResetSystemLib|OvmfPkg/RiscVVirt/Library/ResetSystemLib/BaseResetSystemLib.inf
  RngLib|MdePkg/Library/BaseRngLibNull/BaseRngLibNull.inf

[PcdsFixedAtBuild]
  # Keep the fallback time saved by previous SG2042 test builds.
  gRuyiPlatformTokenSpaceGuid.PcdVirtualRtcTimeVariableName|L"Sg2042VirtualTime"
  gEfiMdeModulePkgTokenSpaceGuid.PcdFrontPageLogoEnable|TRUE
  # Match RiscVVirt: avoid walking entire lists on every network/pool operation.
  gEfiMdePkgTokenSpaceGuid.PcdMaximumLinkedListLength|0
  gRuyiPlatformTokenSpaceGuid.PcdDriverEmulationAvailable|$(X64EMU_ENABLE)
  # C920 uses XTheadMae for page memory types.
  gEfiMdePkgTokenSpaceGuid.PcdRiscVFeatureOverride|0
  gUefiCpuPkgTokenSpaceGuid.PcdRiscVPlatformHasTHeadMae|TRUE
  gUefiCpuPkgTokenSpaceGuid.PcdRiscVRequireMmu|TRUE
  gUefiCpuPkgTokenSpaceGuid.PcdCpuRiscVMmuMaxSatpMode|8
  gUefiRiscVPlatformPkgTokenSpaceGuid.PcdRiscVPeiBootEnable|FALSE
  gUefiRiscVPlatformPkgTokenSpaceGuid.PcdForceNoAcpi|TRUE
  gUefiRiscVPlatformPkgTokenSpaceGuid.PcdTemporaryRamBase64|0x04000000
  gUefiRiscVPlatformPkgTokenSpaceGuid.PcdTemporaryRamSize|0x02000000
  gUefiCpuPkgTokenSpaceGuid.PcdPeiTemporaryRamStackSize|0x10000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableSize|0x40000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingSize|0x10000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareSize|0x40000
  gEfiMdeModulePkgTokenSpaceGuid.PcdVariableStoreSize|0x40000
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxVariableSize|0x2000
  gEfiMdeModulePkgTokenSpaceGuid.PcdResetOnMemoryTypeInformationChange|FALSE
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultBaudRate|115200
  gEfiMdePkgTokenSpaceGuid.PcdDefaultTerminalType|4
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootManagerMenuFile|{0x21,0xaa,0x2c,0x46,0x14,0x76,0x03,0x45,0x83,0x6e,0x8a,0xb6,0xf4,0x66,0x23,0x31}
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesCode|0x80
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesData|0x200
  gEfiMdeModulePkgTokenSpaceGuid.PcdSmbiosEntryPointProvideMethod|0x2

[PcdsDynamicDefault]
  gSg2042PlatformTokenSpaceGuid.PcdSg2042FlashUid|{0}|VOID*|16
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableBase64|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingBase64|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareBase64|0

[PcdsDynamicHii]
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|L"Timeout"|gEfiGlobalVariableGuid|0x0|5

[BuildOptions.common.EDKII.DXE_CORE, BuildOptions.common.EDKII.DXE_DRIVER, BuildOptions.common.EDKII.DXE_RUNTIME_DRIVER, BuildOptions.common.EDKII.UEFI_DRIVER, BuildOptions.common.EDKII.UEFI_APPLICATION]
  GCC:*_*_*_DLINK_FLAGS = -z common-page-size=0x1000

[Components]
  RuyiPlatformPkg/Drivers/PlatformSettingsDxe/VariableSettingsDxe.inf
  RuyiPlatformPkg/Drivers/PlatformDefaultsUiDxe/PlatformDefaultsUiDxe.inf
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf {
    <LibraryClasses>
!if $(X64EMU_ENABLE) == TRUE
      NULL|RuyiPlatformPkg/Library/PciGopSecurityLib/PciGopSecurityLib.inf
!endif
  }
  SophgoPlatformPkg/Drivers/Sg2042PciDxe/Sg2042PciDxe.inf
  RuyiPlatformPkg/Drivers/PciCpuIo2Dxe/PciCpuIo2Dxe.inf
  MdeModulePkg/Bus/Pci/PciHostBridgeDxe/PciHostBridgeDxe.inf
  MdeModulePkg/Bus/Pci/PciBusDxe/PciBusDxe.inf
  MdeModulePkg/Bus/Pci/NvmExpressDxe/NvmExpressDxe.inf
  SophgoPlatformPkg/Drivers/Sg2042MmcDxe/Sg2042MmcDxe.inf
  SophgoPlatformPkg/Drivers/Sg2042MacDxe/Sg2042MacDxe.inf
  RuyiPlatformPkg/Drivers/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.inf
  MdeModulePkg/Bus/Pci/SdMmcPciHcDxe/SdMmcPciHcDxe.inf
  MdeModulePkg/Bus/Sd/SdDxe/SdDxe.inf
  MdeModulePkg/Bus/Sd/EmmcDxe/EmmcDxe.inf
  MdeModulePkg/Bus/Pci/SataControllerDxe/SataControllerDxe.inf
  MdeModulePkg/Bus/Ata/AtaAtapiPassThru/AtaAtapiPassThru.inf
  MdeModulePkg/Bus/Ata/AtaBusDxe/AtaBusDxe.inf
  MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf
  MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf

[Components]
  SophgoPlatformPkg/Drivers/Sg2042SmbiosDxe/Sg2042SmbiosDxe.inf
  RuyiPlatformPkg/Drivers/SystemInfoDxe/SystemInfoDxe.inf

[LibraryClasses.common.DXE_DRIVER]
  PlatformBootManagerLib|RuyiPlatformPkg/Library/BdsBootManagerLib/BdsBootManagerLib.inf

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
[Components]
  RuyiPlatformPkg/Drivers/Rtl812xSnpDxe/Rtl812xSnpDxe.inf
!endif
!include RuyiPlatformPkg/Network.dsc.inc

[Components]
  SophgoPlatformPkg/Drivers/Sg2042NvDxe/Sg2042NvDxe.inf
  RuyiPlatformPkg/Drivers/LogoDxe/LogoDxe.inf
  MdeModulePkg/Universal/Acpi/BootGraphicsResourceTableDxe/BootGraphicsResourceTableDxe.inf
  MdeModulePkg/Universal/FaultTolerantWriteDxe/FaultTolerantWriteDxe.inf {
    <LibraryClasses>
      NULL|RuyiPlatformPkg/Library/NvReadyLib/FtwReadyLib.inf
  }
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf {
    <LibraryClasses>
      NULL|RuyiPlatformPkg/Library/NvReadyLib/NvReadyLib.inf
  }

[Components]
  MdeModulePkg/Application/UiApp/UiApp.inf {
    <LibraryClasses>
      NULL|RuyiPlatformPkg/Library/FirmwareUpdateUiLib/FirmwareUpdateUiLib.inf
      NULL|MdeModulePkg/Library/DeviceManagerUiLib/DeviceManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootManagerUiLib/BootManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootMaintenanceManagerUiLib/BootMaintenanceManagerUiLib.inf
      ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
  }
