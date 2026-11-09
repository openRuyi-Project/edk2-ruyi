## SPDX-License-Identifier: BSD-2-Clause-Patent
[Defines]
  PLATFORM_NAME = SiFivePremierP550
  PLATFORM_GUID = 1d6d21ab-aee7-474b-80c5-c023ec78e4a6
  PLATFORM_VERSION = 0.1
  DSC_SPECIFICATION = 0x0001001C
  OUTPUT_DIRECTORY = Build/SiFivePremierP550
  SUPPORTED_ARCHITECTURES = RISCV64
  BUILD_TARGETS = DEBUG|RELEASE
  SKUID_IDENTIFIER = DEFAULT
  FLASH_DEFINITION = EswinPlatformPkg/PremierP550/PremierP550.fdf
  DEFINE RISCV_PEI_BOOTING = FALSE
  DEFINE SECURE_BOOT_ENABLE = FALSE
  DEFINE DEBUG_ON_SERIAL_PORT = TRUE
  DEFINE HTTP_BOOT_ENABLE = FALSE

!include RuyiPlatformPkg/NetworkDefines.dsc.inc
!include Silicon/RISC-V/RiscV.dsc.inc

[LibraryClasses]
  NvFvbLib|RuyiPlatformPkg/Library/NvFvbLib/NvFvbLib.inf
  NvStorageLib|RuyiPlatformPkg/Library/NvStorageLib/NvStorageLib.inf
  Eic7700SpiNorLib|EswinPlatformPkg/Library/Eic7700SpiNorLib/Eic7700SpiNorLib.inf
  PremierP550NvFlashLib|EswinPlatformPkg/Library/PremierP550NvFlashLib/PremierP550NvFlashLib.inf
  FastbootTransportLib|RuyiPlatformPkg/Library/Dwc3FastbootLib/Dwc3FastbootLib.inf
  FastbootStorageLib|RuyiPlatformPkg/Library/FastbootStorageLib/FastbootStorageLib.inf
  FastbootPlatformLib|EswinPlatformPkg/Library/Eic7700FastbootPlatformLib/Eic7700FastbootPlatformLib.inf
  UsbDevicePlatformLib|EswinPlatformPkg/Library/PremierP550UsbDevicePlatformLib/PremierP550UsbDevicePlatformLib.inf
  Eic7700UsbLib|EswinPlatformPkg/Library/Eic7700UsbLib/Eic7700UsbLib.inf
  EqosLib|RuyiPlatformPkg/Library/EqosLib/EqosLib.inf
  EqosPlatformLib|EswinPlatformPkg/Library/Eic7700EqosPlatformLib/Eic7700EqosPlatformLib.inf
  PlatformMacAddressLib|EswinPlatformPkg/Library/PremierP550MacLib/PremierP550MacLib.inf
  SnpAdapterInfoLib|RuyiPlatformPkg/Library/SnpAdapterInfoLib/SnpAdapterInfoLib.inf
  DmaLib|EswinPlatformPkg/Library/Eic7700DmaLib/Eic7700DmaLib.inf
  NonDiscoverableDeviceRegistrationLib|MdeModulePkg/Library/NonDiscoverableDeviceRegistrationLib/NonDiscoverableDeviceRegistrationLib.inf
  UefiUsbLib|MdePkg/Library/UefiUsbLib/UefiUsbLib.inf
  FdtHardwareLib|RuyiPlatformPkg/Library/FdtHardwareLib/FdtHardwareLib.inf
  Eic7700InfoLib|EswinPlatformPkg/Library/Eic7700InfoLib/Eic7700InfoLib.inf
  PlatformCpuInfoLib|EswinPlatformPkg/Library/Eic7700CpuInfoLib/Eic7700CpuInfoLib.inf
  FdtBoardInfoLib|RuyiPlatformPkg/Library/FdtBoardInfoLib/FdtBoardInfoLib.inf
  BaseCryptLib|CryptoPkg/Library/BaseCryptLib/BaseCryptLib.inf
  OpensslLib|CryptoPkg/Library/OpensslLib/OpensslLib.inf
  IntrinsicLib|CryptoPkg/Library/IntrinsicLib/IntrinsicLib.inf
  BootManagerLib|RuyiPlatformPkg/Library/BootManagerLib/BootManagerLib.inf
  BootProgressLib|RuyiPlatformPkg/Library/BootProgressLib/BootProgressLib.inf
  BootLogoLib|MdeModulePkg/Library/BootLogoLib/BootLogoLib.inf
  BmpSupportLib|MdeModulePkg/Library/BaseBmpSupportLib/BaseBmpSupportLib.inf
  NetworkBootConfigLib|RuyiPlatformPkg/Library/VariableNetworkBootConfigLib/VariableNetworkBootConfigLib.inf
  PciGopLoaderLib|RuyiPlatformPkg/Library/PciGopLoaderLibNull/PciGopLoaderLibNull.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  OemHookStatusCodeLib|MdeModulePkg/Library/OemHookStatusCodeLibNull/OemHookStatusCodeLibNull.inf
  ResetSystemLib|OvmfPkg/RiscVVirt/Library/ResetSystemLib/BaseResetSystemLib.inf
  RngLib|MdePkg/Library/BaseRngLibNull/BaseRngLibNull.inf

[LibraryClasses.common.DXE_DRIVER]
  PlatformBootManagerLib|RuyiPlatformPkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.inf

[LibraryClasses.common.DXE_RUNTIME_DRIVER]
  DebugLib|MdePkg/Library/DxeRuntimeDebugLibSerialPort/DxeRuntimeDebugLibSerialPort.inf

[PcdsFixedAtBuild]
  gEfiMdeModulePkgTokenSpaceGuid.PcdFrontPageLogoEnable|TRUE
  gEfiMdePkgTokenSpaceGuid.PcdMaximumLinkedListLength|0
  # EIC7700 P550 cores have Sv48, without Svpbmt or Zicbom.
  gEfiMdePkgTokenSpaceGuid.PcdRiscVFeatureOverride|0
  gUefiCpuPkgTokenSpaceGuid.PcdCpuRiscVMmuMaxSatpMode|9
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
  EmbeddedPkg/Application/AndroidFastboot/AndroidFastbootApp.inf
  RuyiPlatformPkg/Drivers/FastbootDxe/FastbootDxe.inf
  EswinPlatformPkg/Drivers/PremierP550FdtDxe/PremierP550FdtDxe.inf
  RuyiPlatformPkg/Drivers/EqosSnpDxe/EqosSnpDxe.inf
  EswinPlatformPkg/Drivers/Eic7700SdhciDxe/Eic7700SdhciDxe.inf
  MdeModulePkg/Bus/Pci/SdMmcPciHcDxe/SdMmcPciHcDxe.inf
  MdeModulePkg/Bus/Sd/SdDxe/SdDxe.inf
  MdeModulePkg/Bus/Sd/EmmcDxe/EmmcDxe.inf
  EswinPlatformPkg/Drivers/Eic7700UsbHostDxe/Eic7700UsbHostDxe.inf
  RuyiPlatformPkg/Drivers/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.inf
  MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf
  MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf
  MdeModulePkg/Universal/Disk/UdfDxe/UdfDxe.inf
  EswinPlatformPkg/Drivers/PremierP550SmbiosDxe/PremierP550SmbiosDxe.inf
  RuyiPlatformPkg/Drivers/SystemInfoDxe/SystemInfoDxe.inf
  SecurityPkg/Hash2DxeCrypto/Hash2DxeCrypto.inf
  MdeModulePkg/Application/BootManagerMenuApp/BootManagerMenuApp.inf
  RuyiPlatformPkg/Drivers/LogoDxe/LogoDxe.inf
  MdeModulePkg/Universal/Acpi/BootGraphicsResourceTableDxe/BootGraphicsResourceTableDxe.inf

!include RuyiPlatformPkg/Network.dsc.inc

[Components]
  EswinPlatformPkg/Drivers/PremierP550NvStorageDxe/PremierP550NvStorageDxe.inf
  MdeModulePkg/Universal/FaultTolerantWriteDxe/FaultTolerantWriteDxe.inf {
    <LibraryClasses>
      NULL|RuyiPlatformPkg/Library/NvReadyLib/FtwReadyLib.inf
  }
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf {
    <LibraryClasses>
      NULL|RuyiPlatformPkg/Library/NvReadyLib/NvReadyLib.inf
  }
