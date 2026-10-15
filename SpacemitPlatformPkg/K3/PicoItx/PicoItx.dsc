## SPDX-License-Identifier: BSD-2-Clause-Patent
[Defines]
  PLATFORM_NAME = SpacemitK3PicoItx
  PLATFORM_GUID = 9f958f5d-c809-47f8-a3ba-ff6a1b01c836
  PLATFORM_VERSION = 0.1
  DSC_SPECIFICATION = 0x0001001C
  DEFINE X64EMU_ENABLE = TRUE
  OUTPUT_DIRECTORY = Build/SpacemitK3PicoItx
  SUPPORTED_ARCHITECTURES = RISCV64
  BUILD_TARGETS = DEBUG|RELEASE
  SKUID_IDENTIFIER = DEFAULT
  FLASH_DEFINITION = SpacemitPlatformPkg/K3/PicoItx/PicoItx.fdf
  DEFINE K3_GOP_ENABLE = TRUE
  DEFINE K3_UFS_ENABLE = TRUE
  DEFINE K3_PCIE_ENABLE = TRUE
  DEFINE K3_USB_HOST_ENABLE = TRUE
!if $(TARGET) == DEBUG
  DEFINE DEBUG_PRINT_ERROR_LEVEL = 0x80000047
!else
  DEFINE DEBUG_PRINT_ERROR_LEVEL = 0x80000002
!endif
  DEFINE NETWORK_ENABLE = TRUE
  # Native MAC drivers publish SNP directly; the generic UNDI adapter is not RISCV64.
  DEFINE NETWORK_SNP_ENABLE = FALSE
  DEFINE NETWORK_IP4_ENABLE = TRUE
  DEFINE NETWORK_IP6_ENABLE = TRUE
  DEFINE NETWORK_PXE_BOOT_ENABLE = TRUE
  DEFINE NETWORK_TLS_ENABLE = TRUE
  DEFINE NETWORK_HTTP_BOOT_ENABLE = TRUE
  DEFINE NETWORK_ALLOW_HTTP_CONNECTIONS = TRUE
  DEFINE NETWORK_ISCSI_ENABLE = FALSE

!include MdePkg/MdeLibs.dsc.inc
!include NetworkPkg/Network.dsc.inc

[LibraryClasses]
  K3GopPolicyLib|SpacemitPlatformPkg/Library/K3GopPolicyLib/K3GopPolicyLib.inf
!if $(X64EMU_ENABLE) == TRUE
  K3GopLoaderLib|SpacemitPlatformPkg/Library/K3GopLoaderLib/K3GopLoaderLib.inf
!else
  K3GopLoaderLib|SpacemitPlatformPkg/Library/K3GopLoaderLibNull/K3GopLoaderLibNull.inf
!endif
  K3QspiLib|SpacemitPlatformPkg/Library/K3QspiLib/K3QspiLib.inf
  BootProgressLib|SpacemitPlatformPkg/Library/BootProgressLib/BootProgressLib.inf
  BootLogoLib|MdeModulePkg/Library/BootLogoLib/BootLogoLib.inf
  BmpSupportLib|MdeModulePkg/Library/BaseBmpSupportLib/BaseBmpSupportLib.inf
  SnpAdapterInfoLib|SpacemitPlatformPkg/Library/SnpAdapterInfoLib/SnpAdapterInfoLib.inf
  Rtl8127SnpLib|SpacemitPlatformPkg/Library/Rtl8127SnpLib/Rtl8127SnpLib.inf
  PlatformMacAddressLib|SpacemitPlatformPkg/Library/K3BoardMacLib/K3BoardMacLib.inf
  SpacemitTlvLib|SpacemitPlatformPkg/Library/SpacemitTlvLib/SpacemitTlvLib.inf
  K3BoardEepromLib|SpacemitPlatformPkg/Library/K3BoardEepromLib/K3BoardEepromLib.inf
  Rtl8127Lib|SpacemitPlatformPkg/Library/Rtl8127Lib/Rtl8127Lib.inf
  Rtl8127PacketsLib|SpacemitPlatformPkg/Library/Rtl8127PacketsLib/Rtl8127PacketsLib.inf
  K3SpiNorLib|SpacemitPlatformPkg/Library/K3SpiNorLib/K3SpiNorLib.inf
  K3SpiConfigLib|SpacemitPlatformPkg/Library/K3SpiConfigLib/K3SpiConfigLib.inf
  K3CpuConfigLib|SpacemitPlatformPkg/Library/K3CpuConfigLib/K3CpuConfigLib.inf
  K3FirmwareUpdateLib|SpacemitPlatformPkg/Library/K3FirmwareUpdateLib/K3FirmwareUpdateLib.inf
  K3NvFvbLib|SpacemitPlatformPkg/Library/K3NvFvbLib/K3NvFvbLib.inf
  UfsStorageLib|SpacemitPlatformPkg/Library/UfsStorageLib/UfsStorageLib.inf
  BaseCryptLib|CryptoPkg/Library/BaseCryptLib/BaseCryptLib.inf
  OpensslLib|CryptoPkg/Library/OpensslLib/OpensslLib.inf
  IntrinsicLib|CryptoPkg/Library/IntrinsicLib/IntrinsicLib.inf
  TlsLib|CryptoPkg/Library/TlsLib/TlsLib.inf
  RngLib|MdePkg/Library/DxeRngLib/DxeRngLib.inf
  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf
  K3DisplayModeLib|SpacemitPlatformPkg/Library/K3DisplayModeLib/K3DisplayModeLib.inf
  K3GopLib|SpacemitPlatformPkg/Library/K3GopLib/K3GopLib.inf
  K3PowerDomainLib|SpacemitPlatformPkg/Library/K3PowerDomainLib/K3PowerDomainLib.inf
  K3DisplayClockLib|SpacemitPlatformPkg/Library/K3DisplayClockLib/K3DisplayClockLib.inf
  K3DisplaySurfaceLib|SpacemitPlatformPkg/Library/K3DisplaySurfaceLib/K3DisplaySurfaceLib.inf
  K3DpuLib|SpacemitPlatformPkg/Library/K3DpuLib/K3DpuLib.inf
  FrameBufferBltLib|MdeModulePkg/Library/FrameBufferBltLib/FrameBufferBltLib.inf
  K3InnoDpLib|SpacemitPlatformPkg/Library/K3InnoDpLib/K3InnoDpLib.inf
  UefiUsbLib|MdePkg/Library/UefiUsbLib/UefiUsbLib.inf
  GptLib|MdeModulePkg/Library/GptLib/GptLib.inf
  NonDiscoverableDeviceRegistrationLib|MdeModulePkg/Library/NonDiscoverableDeviceRegistrationLib/NonDiscoverableDeviceRegistrationLib.inf
  K3HardwareLib|SpacemitPlatformPkg/Library/K3HardwareLib/K3HardwareLib.inf
  K3EcLib|SpacemitPlatformPkg/Library/K3EcLib/K3EcLib.inf
  K3EqosLib|SpacemitPlatformPkg/Library/K3EqosLib/K3EqosLib.inf
  DesignWarePciLib|SpacemitPlatformPkg/Library/DesignWarePciLib/DesignWarePciLib.inf
  PciSegmentLib|SpacemitPlatformPkg/Library/SpacemitPciSegmentLib/SpacemitPciSegmentLib.inf
  PciHostBridgeLib|SpacemitPlatformPkg/Library/SpacemitPciHostBridgeLib/SpacemitPciHostBridgeLib.inf
  DmaLib|SpacemitPlatformPkg/Library/SpacemitDmaLib/SpacemitDmaLib.inf
  FastbootTransportLib|SpacemitPlatformPkg/Library/Dwc3FastbootLib/Dwc3FastbootLib.inf
  FastbootPlatformLib|SpacemitPlatformPkg/Library/K3FastbootPlatformLib/K3FastbootPlatformLib.inf
  FastbootStorageLib|SpacemitPlatformPkg/Library/FastbootStorageLib/FastbootStorageLib.inf
  UsbDevicePlatformLib|SpacemitPlatformPkg/Library/K3UsbDevicePlatformLib/K3UsbDevicePlatformLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  CpuLib|MdePkg/Library/BaseCpuLib/BaseCpuLib.inf
  CacheMaintenanceLib|MdePkg/Library/BaseCacheMaintenanceLib/BaseCacheMaintenanceLib.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  PeCoffLib|MdePkg/Library/BasePeCoffLib/BasePeCoffLib.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf
  PeCoffExtraActionLib|MdePkg/Library/BasePeCoffExtraActionLibNull/BasePeCoffExtraActionLibNull.inf
  UefiDecompressLib|MdePkg/Library/BaseUefiDecompressLib/BaseUefiDecompressLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiRuntimeLib|MdePkg/Library/UefiRuntimeLib/UefiRuntimeLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
  DxeServicesLib|MdePkg/Library/DxeServicesLib/DxeServicesLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLibDevicePathProtocol/UefiDevicePathLibDevicePathProtocol.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf
  PerformanceLib|MdePkg/Library/BasePerformanceLibNull/BasePerformanceLibNull.inf
  ImagePropertiesRecordLib|MdeModulePkg/Library/ImagePropertiesRecordLib/ImagePropertiesRecordLib.inf
  OemHookStatusCodeLib|MdeModulePkg/Library/OemHookStatusCodeLibNull/OemHookStatusCodeLibNull.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
  RiscVSbiLib|MdePkg/Library/BaseRiscVSbiLib/BaseRiscVSbiLib.inf
  RiscVMmuLib|UefiCpuPkg/Library/BaseRiscVMmuLib/BaseRiscVMmuLib.inf
  CpuExceptionHandlerLib|UefiCpuPkg/Library/CpuExceptionHandlerLib/DxeCpuExceptionHandlerLib.inf
  TimerLib|UefiCpuPkg/Library/RiscV64CpuTimerLib/RiscV64CpuTimerDxeLib.inf
  SerialPortLib|MdePkg/Library/BaseSerialPortLibRiscVSbiLib/BaseSerialPortLibRiscVSbiLibRam.inf
  UefiBootManagerLib|MdeModulePkg/Library/UefiBootManagerLib/UefiBootManagerLib.inf
  PlatformBootManagerLib|SpacemitPlatformPkg/Library/BootManagerLib/BootManagerLib.inf
  ResetSystemLib|OvmfPkg/RiscVVirt/Library/ResetSystemLib/BaseResetSystemLib.inf
  SecurityManagementLib|MdeModulePkg/Library/DxeSecurityManagementLib/DxeSecurityManagementLib.inf
  TpmMeasurementLib|MdeModulePkg/Library/TpmMeasurementLibNull/TpmMeasurementLibNull.inf
  AuthVariableLib|MdeModulePkg/Library/AuthVariableLibNull/AuthVariableLibNull.inf
  VarCheckLib|MdeModulePkg/Library/VarCheckLib/VarCheckLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLib.inf
  VariablePolicyHelperLib|MdeModulePkg/Library/VariablePolicyHelperLib/VariablePolicyHelperLib.inf
  VariableFlashInfoLib|MdeModulePkg/Library/BaseVariableFlashInfoLib/BaseVariableFlashInfoLib.inf
  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibNull/DxeCapsuleLibNull.inf
  # P1 timekeeping belongs to the OS; do not expose a build-time clock.
  RealTimeClockLib|EmbeddedPkg/Library/TemplateRealTimeClockLib/TemplateRealTimeClockLib.inf
  TimeBaseLib|EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf
  FileExplorerLib|MdeModulePkg/Library/FileExplorerLib/FileExplorerLib.inf
  CustomizedDisplayLib|MdeModulePkg/Library/CustomizedDisplayLib/CustomizedDisplayLib.inf
  UefiHiiServicesLib|MdeModulePkg/Library/UefiHiiServicesLib/UefiHiiServicesLib.inf
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
  HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
  BcfgCommandLib|ShellPkg/Library/UefiShellBcfgCommandLib/UefiShellBcfgCommandLib.inf
  ShellCEntryLib|ShellPkg/Library/UefiShellCEntryLib/UefiShellCEntryLib.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  SortLib|MdeModulePkg/Library/UefiSortLib/UefiSortLib.inf
  OrderedCollectionLib|MdePkg/Library/BaseOrderedCollectionRedBlackTreeLib/BaseOrderedCollectionRedBlackTreeLib.inf
  SafeIntLib|MdePkg/Library/BaseSafeIntLib/BaseSafeIntLib.inf
  DebugAgentLib|MdeModulePkg/Library/DebugAgentLibNull/DebugAgentLibNull.inf

[LibraryClasses.common.SEC]
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf
  CpuExceptionHandlerLib|UefiCpuPkg/Library/CpuExceptionHandlerLib/SecPeiCpuExceptionHandlerLib.inf
  SerialPortLib|MdePkg/Library/BaseSerialPortLibRiscVSbiLib/BaseSerialPortLibRiscVSbiLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  HobLib|EmbeddedPkg/Library/PrePiHobLib/PrePiHobLib.inf
  MemoryAllocationLib|EmbeddedPkg/Library/PrePiMemoryAllocationLib/PrePiMemoryAllocationLib.inf
  ExtractGuidedSectionLib|EmbeddedPkg/Library/PrePiExtractGuidedSectionLib/PrePiExtractGuidedSectionLib.inf
  TimerLib|UefiCpuPkg/Library/RiscV64CpuTimerLib/RiscV64CpuTimerSecLib.inf
  PrePiLib|EmbeddedPkg/Library/PrePiLib/PrePiLib.inf
  PrePiHobListPointerLib|OvmfPkg/RiscVVirt/Library/PrePiHobListPointerLib/PrePiHobListPointerLib.inf

[LibraryClasses.common.DXE_CORE]
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  MemoryAllocationLib|MdeModulePkg/Library/DxeCoreMemoryAllocationLib/DxeCoreMemoryAllocationLib.inf
  HobLib|MdePkg/Library/DxeCoreHobLib/DxeCoreHobLib.inf
  DxeCoreEntryPoint|MdePkg/Library/DxeCoreEntryPoint/DxeCoreEntryPoint.inf

[LibraryClasses.common.DXE_RUNTIME_DRIVER]
  DebugLib|MdePkg/Library/DxeRuntimeDebugLibSerialPort/DxeRuntimeDebugLibSerialPort.inf
  ReportStatusCodeLib|MdeModulePkg/Library/RuntimeDxeReportStatusCodeLib/RuntimeDxeReportStatusCodeLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLibRuntimeDxe.inf

[PcdsFixedAtBuild]
  # K3 DRAM starts above 4 GiB; SMBIOS 2.x cannot address its tables.
  gEfiMdeModulePkgTokenSpaceGuid.PcdSmbiosEntryPointProvideMethod|0x2
  # Runtime code/data measured at 236/1512 KiB, excluding the bounded GOP buffer.
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesCode|0x40
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesData|0x200
  gEfiMdePkgTokenSpaceGuid.PcdEnforceSecureRngAlgorithms|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdVariableStoreSize|0x80000
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxVolatileVariableSize|0x40000
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxAuthVariableSize|0x8400
  # Standard UiApp is the firmware setup entry.
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootManagerMenuFile|{0x21,0xaa,0x2c,0x46,0x14,0x76,0x03,0x45,0x83,0x6e,0x8a,0xb6,0xf4,0x66,0x23,0x31}
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2f
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|$(DEBUG_PRINT_ERROR_LEVEL)
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableSize|0x40000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingSize|0x10000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareSize|0x40000
  gEfiMdeModulePkgTokenSpaceGuid.PcdResetOnMemoryTypeInformationChange|FALSE
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultBaudRate|115200
  # K3 provides Zicbo and Svpbmt; clean OpenSBI enables the S-mode controls.
  gEfiMdePkgTokenSpaceGuid.PcdRiscVFeatureOverride|5
  gUefiCpuPkgTokenSpaceGuid.PcdCpuCoreCrystalClockFrequency|24000000
  gUefiCpuPkgTokenSpaceGuid.PcdCpuRiscVMmuMaxSatpMode|8
  gSpacemitPlatformTokenSpaceGuid.PcdFdBase|0x102000000
  gSpacemitPlatformTokenSpaceGuid.PcdFdSize|0x300000
  gSpacemitPlatformTokenSpaceGuid.PcdSecMemoryBase|0x122000000
  gSpacemitPlatformTokenSpaceGuid.PcdSecMemorySize|0x2000000
  gSpacemitPlatformTokenSpaceGuid.PcdSecStackSize|0x10000
  gSpacemitPlatformTokenSpaceGuid.PcdSystemProductName|"SpacemiT K3 Pico ITX"
  gSpacemitPlatformTokenSpaceGuid.PcdDramDeviceCount|2

[PcdsDynamicDefault]
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableBase64|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingBase64|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareBase64|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutGlyphScale|4
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoHorizontalResolution|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoVerticalResolution|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutColumn|80
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutRow|25
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoHorizontalResolution|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoVerticalResolution|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutColumn|80
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutRow|25
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|5
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVendor|L"SpacemiT"
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString|L"EDK II stable202608"

!if $(X64EMU_ENABLE) == TRUE
[PcdsDynamicExDefault]
  gMauPkgTokenSpaceGuid.PcdMauEmulationEnabled|FALSE
!endif

[PcdsFeatureFlag]
  # Allow 64-bit BARs for UEFI option ROMs.
  gEfiMdeModulePkgTokenSpaceGuid.PcdPciDegradeResourceForOptionRom|FALSE
  # OS handoff uses FDT/UEFI protocols; no runtime HII export consumer.
  gEfiMdeModulePkgTokenSpaceGuid.PcdHiiOsRuntimeSupport|FALSE

[BuildOptions.common.EDKII.DXE_CORE, BuildOptions.common.EDKII.DXE_DRIVER, BuildOptions.common.EDKII.DXE_RUNTIME_DRIVER, BuildOptions.common.EDKII.UEFI_DRIVER, BuildOptions.common.EDKII.UEFI_APPLICATION]
  GCC:*_*_*_DLINK_FLAGS = -z common-page-size=0x1000

!if $(K3_USB_HOST_ENABLE) == TRUE
[BuildOptions]
  GCC:*_*_*_CC_FLAGS = -DK3_USB_HOST_STACK
!endif
!if $(X64EMU_ENABLE) == TRUE
[BuildOptions]
  GCC:*_*_*_CC_FLAGS = -DK3_X64EMU
  GCC:*_*_*_VFRPP_FLAGS = -DK3_X64EMU
!endif

[Components]
!if $(X64EMU_ENABLE) == TRUE
  SpacemitPlatformPkg/Drivers/X64EmulatorDxe/X64EmulatorDxe.inf
!endif
!if $(NETWORK_ENABLE) == TRUE
  SecurityPkg/Hash2DxeCrypto/Hash2DxeCrypto.inf
!endif
  SpacemitPlatformPkg/Drivers/K3QspiDxe/K3QspiDxe.inf
  MdeModulePkg/Bus/Spi/SpiNorFlashJedecSfdp/SpiNorFlashJedecSfdpDxe.inf
  SpacemitPlatformPkg/Drivers/K3DdrTrainingDxe/K3DdrTrainingDxe.inf
  SpacemitPlatformPkg/Drivers/TlvEepromCommandDxe/TlvEepromCommandDxe.inf {
    <PcdsFixedAtBuild>
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
  }
  SpacemitPlatformPkg/Drivers/K3ClockDxe/K3ClockDxe.inf
!if $(NETWORK_ENABLE) == TRUE
  SpacemitPlatformPkg/Drivers/K3EqosSnpDxe/K3EqosSnpDxe.inf
  SpacemitPlatformPkg/Drivers/Rtl8127SnpDxe/Rtl8127SnpDxe.inf
!endif
!if $(K3_PCIE_ENABLE) == TRUE || $(K3_USB_HOST_ENABLE) == TRUE
  SpacemitPlatformPkg/Drivers/K3ComboPhyDxe/K3ComboPhyDxe.inf
!endif
!if $(K3_USB_HOST_ENABLE) == TRUE
  SpacemitPlatformPkg/Drivers/K3UsbHostDxe/K3UsbHostDxe.inf
  SpacemitPlatformPkg/Drivers/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.inf
  MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf
  MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf
!endif
!if $(K3_PCIE_ENABLE) == TRUE || $(K3_USB_HOST_ENABLE) == TRUE || $(K3_UFS_ENABLE) == TRUE
  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf
!endif
!if $(K3_UFS_ENABLE) == TRUE
  SpacemitPlatformPkg/Drivers/K3UfsHcDxe/K3UfsHcDxe.inf
  MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf
!endif
!if $(K3_PCIE_ENABLE) == TRUE
  SpacemitPlatformPkg/Drivers/K3PciControllerDxe/K3PciControllerDxe.inf
  SpacemitPlatformPkg/Drivers/PciCpuIo2Dxe/CpuIo2Dxe.inf
  SpacemitPlatformPkg/Drivers/PciHostBridgeDxe/PciHostBridgeDxe.inf
  MdeModulePkg/Bus/Pci/PciBusDxe/PciBusDxe.inf
  MdeModulePkg/Bus/Pci/NvmExpressDxe/NvmExpressDxe.inf
!endif
  SpacemitPlatformPkg/Application/Fastboot/Fastboot.inf {
    <LibraryClasses>
      # Firmware integrity checks only use deterministic SHA-256.
      RngLib|MdePkg/Library/BaseRngLibNull/BaseRngLibNull.inf
  }
  SpacemitPlatformPkg/Sec/Sec.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
  }
  MdeModulePkg/Core/Dxe/DxeMain.inf
  MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf {
    <LibraryClasses>
      DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
      PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  }
  MdeModulePkg/Universal/PCD/Dxe/Pcd.inf {
    <LibraryClasses>
      DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
      PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  }
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf {
    <LibraryClasses>
      NULL|SpacemitPlatformPkg/Library/K3GopSecurityLib/K3GopSecurityLib.inf
      RngLib|MdePkg/Library/BaseRngLibNull/BaseRngLibNull.inf
  }
  UefiCpuPkg/CpuDxeRiscV64/CpuDxeRiscV64.inf
  UefiCpuPkg/CpuTimerDxeRiscV64/CpuTimerDxeRiscV64.inf
  MdeModulePkg/Universal/Metronome/Metronome.inf
  MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  SpacemitPlatformPkg/Drivers/K3NvStorageDxe/K3NvStorageDxe.inf {
!if $(X64EMU_ENABLE) == TRUE
    <LibraryClasses>
      NULL|SpacemitPlatformPkg/Library/K3EmulationPolicyLib/K3EmulationPolicyLib.inf
!endif
  }
  MdeModulePkg/Universal/FaultTolerantWriteDxe/FaultTolerantWriteDxe.inf {
    <LibraryClasses>
      NULL|SpacemitPlatformPkg/Library/K3NvReadyLib/K3FtwReadyLib.inf
  }
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf {
    <LibraryClasses>
      NULL|SpacemitPlatformPkg/Library/K3NvReadyLib/K3NvReadyLib.inf
  }
  MdeModulePkg/Universal/MonotonicCounterRuntimeDxe/MonotonicCounterRuntimeDxe.inf
  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  SpacemitPlatformPkg/Drivers/FdtDxe/FdtDxe.inf
  MdeModulePkg/Universal/SerialDxe/SerialDxe.inf
  MdeModulePkg/Universal/Console/TerminalDxe/TerminalDxe.inf
  MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
!if $(K3_GOP_ENABLE) == TRUE
  SpacemitPlatformPkg/Drivers/K3DpuGopDxe/K3DpuGopDxe.inf
  SpacemitPlatformPkg/Drivers/LogoDxe/LogoDxe.inf
  MdeModulePkg/Universal/Acpi/BootGraphicsResourceTableDxe/BootGraphicsResourceTableDxe.inf
!endif
  MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  MdeModulePkg/Universal/BdsDxe/BdsDxe.inf {
    <LibraryClasses>
      RngLib|MdePkg/Library/BaseRngLibNull/BaseRngLibNull.inf
  }
  MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  MdeModulePkg/Universal/SetupBrowserDxe/SetupBrowserDxe.inf
  MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf
  MdeModulePkg/Universal/SmbiosDxe/SmbiosDxe.inf
  SpacemitPlatformPkg/K3/PicoItx/SmbiosDxe/SmbiosDxe.inf
  SpacemitPlatformPkg/Drivers/K3SystemInfoDxe/K3SystemInfoDxe.inf
  MdeModulePkg/Application/BootManagerMenuApp/BootManagerMenuApp.inf
  MdeModulePkg/Application/UiApp/UiApp.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/DeviceManagerUiLib/DeviceManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootManagerUiLib/BootManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootMaintenanceManagerUiLib/BootMaintenanceManagerUiLib.inf
      NULL|SpacemitPlatformPkg/Library/FirmwareUpdateUiLib/FirmwareUpdateUiLib.inf
      NULL|SpacemitPlatformPkg/Library/PlatformConfigUiLib/PlatformConfigUiLib.inf
      # Firmware integrity checks only use deterministic SHA-256.
      RngLib|MdePkg/Library/BaseRngLibNull/BaseRngLibNull.inf
  }
  MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf
  ShellPkg/Application/Shell/Shell.inf {
    <LibraryClasses>
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDriver1CommandsLib/UefiShellDriver1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDebug1CommandsLib/UefiShellDebug1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellInstall1CommandsLib/UefiShellInstall1CommandsLib.inf
    <PcdsFixedAtBuild>
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
  }
