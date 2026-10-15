/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Guid/FdtHob.h>
#include <Guid/K3MemoryInfo.h>
#include <Guid/RiscVSecHobData.h>
#include <Protocol/Clock.h>
#include <Library/FdtLib.h>
#include <IndustryStandard/SmBios.h>
#include <Protocol/Smbios.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC
EFI_STATUS
AddRecord (
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

STATIC EFI_STATUS
AddProcessor (
  EFI_SMBIOS_PROTOCOL  *Smbios
  )
{
  STATIC CONST EFI_GUID    HandoffGuid = RISCV_SEC_HANDOFF_HOB_GUID;
  CONST CHAR8              *Strings[]  = { "SoC", "SpacemiT", "SpacemiT K3" };
  EFI_HOB_GUID_TYPE        *Hob;
  RISCV_SEC_HANDOFF_DATA   Handoff;
  SPACEMIT_CLOCK_PROTOCOL  *Clock;
  SMBIOS_TABLE_TYPE4       Cpu;
  EFI_SMBIOS_HANDLE        Handle;
  EFI_STATUS               Status;
  UINT64                   Rate = 0;
  UINT16                   Cores = 0, Enabled = 0;
  INT32                    Cpus, Node, Length;
  CONST CHAR8              *Value;

  Hob = GetFirstGuidHob (&HandoffGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (Handoff))) {
    return EFI_NOT_FOUND;
  }

  CopyMem (&Handoff, GET_GUID_HOB_DATA (Hob), sizeof (Handoff));
  if ((Handoff.FdtPointer == NULL) || FdtCheckHeader (Handoff.FdtPointer)) {
    return EFI_COMPROMISED_DATA;
  }

  Cpus = FdtPathOffset (Handoff.FdtPointer, "/cpus");
  if (Cpus < 0) {
    return EFI_NOT_FOUND;
  }

  FdtForEachSubnode (Node, Handoff.FdtPointer, Cpus) {
    Value = FdtGetProp (Handoff.FdtPointer, Node, "device_type", &Length);
    if ((Value == NULL) || (Length != 4) || CompareMem (Value, "cpu", 4)) {
      continue;
    }

    Cores++;
    Value = FdtGetProp (Handoff.FdtPointer, Node, "status", &Length);
    if ((Value == NULL) || ((Length == 5) && !CompareMem (Value, "okay", 5)) ||
        ((Length == 3) && !CompareMem (Value, "ok", 3)))
    {
      Enabled++;
    }
  }
  if ((Cores == 0) || (Enabled == 0)) {
    return EFI_COMPROMISED_DATA;
  }

  Status = gBS->LocateProtocol (&gSpacemitClockProtocolGuid, NULL, (VOID **)&Clock);
  if (!EFI_ERROR (Status)) {
    if ((Clock == NULL) || (Clock->Revision != 1) || (Clock->GetRate == NULL) ||
        (Handoff.BootHartId >= 8))
    {
      Status = EFI_UNSUPPORTED;
    } else {
      // X100 harts share one core clock per four-hart cluster.
      Status = Clock->GetRate (2 + (UINT32)Handoff.BootHartId / 4, &Rate);
    }
  }

  if (EFI_ERROR (Status)) {
    // The provider's output is not valid when its operation fails.
    Rate = 0;
    DEBUG ((DEBUG_WARN, "K3 SMBIOS: CPU clock unavailable: %r\n", Status));
  }

  ZeroMem (&Cpu, sizeof (Cpu));
  Cpu.Hdr.Type              = SMBIOS_TYPE_PROCESSOR_INFORMATION;
  Cpu.Hdr.Length            = sizeof (Cpu);
  Cpu.Socket                = 1;
  Cpu.ProcessorManufacturer = 2;
  Cpu.ProcessorVersion      = 3;
  Cpu.ProcessorType         = CentralProcessor;
  Cpu.ProcessorFamily       = ProcessorFamilyIndicatorFamily2;
  Cpu.ProcessorFamily2      = ProcessorFamilyRiscVRV64;
  Cpu.CurrentSpeed          = (UINT16)MIN (Rate / 1000000, MAX_UINT16);
  Cpu.Status                = 0x41; // Populated socket, enabled processor.
  Cpu.CoreCount             = Cpu.ThreadCount = (UINT8)MIN (Cores, MAX_UINT8);
  Cpu.EnabledCoreCount      = (UINT8)MIN (Enabled, MAX_UINT8);
  Cpu.CoreCount2            = Cpu.ThreadCount2 = Cores;
  Cpu.EnabledCoreCount2     = Cpu.ThreadEnabled = Enabled;
  Cpu.L1CacheHandle         = Cpu.L2CacheHandle = Cpu.L3CacheHandle = 0xffff;
  return AddRecord (Smbios, &Cpu.Hdr, Strings, ARRAY_SIZE (Strings), &Handle);
}

STATIC CONST K3_MEMORY_INFO *
BootMemoryInfo (
  UINT64  Total,
  UINTN   DeviceCount
  )
{
  EFI_HOB_GUID_TYPE     *Hob = GetFirstGuidHob (&gSpacemitMemoryInfoGuid);
  CONST K3_MEMORY_INFO  *Info;
  UINTN                 Length;

  if (!Hob || (GET_GUID_HOB_DATA_SIZE (Hob) != sizeof (*Info))) {
    return NULL;
  }

  Info = GET_GUID_HOB_DATA (Hob);
  if ((Info->Magic != K3_MEMORY_INFO_MAGIC) || (Info->Version != K3_MEMORY_INFO_VERSION) ||
      (Info->Crc32 != CalculateCrc32 ((VOID *)&Info->Version, sizeof (*Info) - 8)) ||
      (Info->DeviceCount != DeviceCount) || !Info->DeviceCount || !Info->DeviceSizeMiB ||
      ((UINT64)Info->DeviceCount * Info->DeviceSizeMiB * SIZE_1MB != Total) ||
      (Info->Type > K3_MEMORY_LPDDR5) || !Info->ChipSelects || (Info->ChipSelects > 15) ||
      !Info->DataRate || (Info->DataRate >= MAX_UINT16))
  {
    return NULL;
  }

  for (Length = 0; Length < sizeof (Info->PartNumber); Length++) {
    if (!Info->PartNumber[Length]) {
      break;
    }

    if ((Info->PartNumber[Length] < 0x20) || (Info->PartNumber[Length] > 0x7e)) {
      return NULL;
    }
  }

  return Length && Length < sizeof (Info->PartNumber) ? Info : NULL;
}

EFI_STATUS
EFIAPI
PublishBoardInfo (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_SMBIOS_PROTOCOL          *Smbios;
  EFI_HOB_GENERIC_HEADER       *Hob;
  EFI_HOB_RESOURCE_DESCRIPTOR  *Resource;
  EFI_SMBIOS_HANDLE            Handle, ArrayHandle;
  SMBIOS_TABLE_TYPE0           Bios;
  SMBIOS_TABLE_TYPE1           System;
  SMBIOS_TABLE_TYPE16          Array;
  SMBIOS_TABLE_TYPE17          Device;
  SMBIOS_TABLE_TYPE19          Range;
  CONST CHAR8                  *BiosStrings[] = { "SpacemiT", "EDK II stable202608" };
  CONST CHAR8                  *SystemStrings[2];
  CONST CHAR8                  *Locator[] = { "Onboard DRAM 0", "Onboard DRAM 1" };
  EFI_STATUS                   Status;
  UINT64                       Total, SizeMiB;
  UINTN                        Index, DeviceCount;
  CONST K3_MEMORY_INFO         *Memory;
  CONST CHAR8                  *MemoryStrings[2];

  Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&Smbios);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Total = 0;
  for (Hob = GetHobList (); !END_OF_HOB_LIST (Hob); Hob = GET_NEXT_HOB (Hob)) {
    if (Hob->HobType != EFI_HOB_TYPE_RESOURCE_DESCRIPTOR) {
      continue;
    }

    Resource = (EFI_HOB_RESOURCE_DESCRIPTOR *)Hob;
    if (Resource->ResourceType != EFI_RESOURCE_SYSTEM_MEMORY) {
      continue;
    }

    if ((Resource->ResourceLength == 0) ||
        (Resource->PhysicalStart > MAX_UINT64 - Resource->ResourceLength) ||
        (Total > MAX_UINT64 - Resource->ResourceLength))
    {
      return EFI_COMPROMISED_DATA;
    }

    Total += Resource->ResourceLength;
  }

  DeviceCount = FixedPcdGet16 (PcdDramDeviceCount);
  if ((Total == 0) || (DeviceCount != ARRAY_SIZE (Locator)) ||
      ((Total % (DeviceCount * SIZE_1MB)) != 0))
  {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (&Bios, sizeof (Bios));
  Bios.Hdr.Type    = SMBIOS_TYPE_BIOS_INFORMATION;
  Bios.Hdr.Length  = sizeof (Bios);
  Bios.Vendor      = 1;
  Bios.BiosVersion = 2;
  /* Report the UEFI firmware device size in 64 KiB units. */
  Bios.BiosSize                               = (UINT8)(FixedPcdGet32 (PcdFdSize) / SIZE_64KB - 1);
  Bios.BIOSCharacteristicsExtensionBytes[1]   = BIT3; /* UEFI supported */
  Bios.EmbeddedControllerFirmwareMajorRelease = 0xff;
  Bios.EmbeddedControllerFirmwareMinorRelease = 0xff;
  Status                                      = AddRecord (
                                                  Smbios,
                                                  &Bios.Hdr,
                                                  BiosStrings,
                                                  ARRAY_SIZE (BiosStrings),
                                                  &Handle
                                                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&System, sizeof (System));
  System.Hdr.Type     = SMBIOS_TYPE_SYSTEM_INFORMATION;
  System.Hdr.Length   = sizeof (System);
  System.Manufacturer = 1;
  System.ProductName  = 2;
  System.WakeUpType   = SystemWakeupTypeUnknown;
  SystemStrings[0]    = "SpacemiT";
  SystemStrings[1]    = FixedPcdGetPtr (PcdSystemProductName);
  Status              = AddRecord (Smbios, &System.Hdr, SystemStrings, ARRAY_SIZE (SystemStrings), &Handle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AddProcessor (Smbios);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "K3 SMBIOS: processor information unavailable: %r\n", Status));
  }

  ZeroMem (&Array, sizeof (Array));
  Array.Hdr.Type                     = SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY;
  Array.Hdr.Length                   = sizeof (Array);
  Array.Location                     = MemoryArrayLocationSystemBoard;
  Array.Use                          = MemoryArrayUseSystemMemory;
  Array.MemoryErrorCorrection        = MemoryErrorCorrectionUnknown;
  Array.MemoryErrorInformationHandle = 0xfffe;
  /* This assembly has soldered DRAM; its array capacity is the installed size. */
  if (Total / SIZE_1KB < 0x80000000ULL) {
    Array.MaximumCapacity = (UINT32)(Total / SIZE_1KB);
  } else {
    Array.MaximumCapacity         = 0x80000000;
    Array.ExtendedMaximumCapacity = Total;
  }

  Array.NumberOfMemoryDevices = (UINT16)DeviceCount;
  Status                      = AddRecord (Smbios, &Array.Hdr, NULL, 0, &ArrayHandle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* Publish the configuration used by coreboot, including quick boots.
   * Capacity still comes from the validated memory resource HOBs.
   */
  Memory  = BootMemoryInfo (Total, DeviceCount);
  SizeMiB = Total / DeviceCount / SIZE_1MB;
  if (SizeMiB > 0x7fffffff) {
    return EFI_UNSUPPORTED;
  }

  for (Index = 0; Index < DeviceCount; Index++) {
    ZeroMem (&Device, sizeof (Device));
    Device.Hdr.Type                     = SMBIOS_TYPE_MEMORY_DEVICE;
    Device.Hdr.Length                   = sizeof (Device);
    Device.MemoryArrayHandle            = ArrayHandle;
    Device.MemoryErrorInformationHandle = 0xfffe;
    /* MT62F2G32D4DS, used by the native DDR configuration, is an x32 package. */
    Device.TotalWidth                                        = Device.DataWidth = 32;
    Device.FormFactor                                        = MemoryFormFactorChip;
    Device.MemoryType                                        = MemoryTypeUnknown;
    Device.MemoryTechnology                                  = MemoryTechnologyDram;
    Device.MemoryOperatingModeCapability.Bits.VolatileMemory = 1;
    Device.VolatileSize                                      = Total / DeviceCount;
    Device.DeviceLocator                                     = 1;
    MemoryStrings[0]                                         = Locator[Index];
    if (Memory != NULL) {
      Device.MemoryType                 = Memory->Type == K3_MEMORY_LPDDR5 ? MemoryTypeLpddr5 : MemoryTypeLpddr4;
      Device.ConfiguredMemoryClockSpeed = (UINT16)Memory->DataRate;
      Device.Attributes                 = (UINT8)Memory->ChipSelects;
      Device.PartNumber                 = 2;
      MemoryStrings[1]                  = Memory->PartNumber;
    }

    Device.Size = SizeMiB < 0x7fff ? (UINT16)SizeMiB : 0x7fff;
    if (Device.Size == 0x7fff) {
      Device.ExtendedSize = (UINT32)SizeMiB;
    }

    Status = AddRecord (Smbios, &Device.Hdr, MemoryStrings, Memory != NULL ? 2 : 1, &Handle);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  for (Hob = GetHobList (); !END_OF_HOB_LIST (Hob); Hob = GET_NEXT_HOB (Hob)) {
    if (Hob->HobType != EFI_HOB_TYPE_RESOURCE_DESCRIPTOR) {
      continue;
    }

    Resource = (EFI_HOB_RESOURCE_DESCRIPTOR *)Hob;
    if (Resource->ResourceType != EFI_RESOURCE_SYSTEM_MEMORY) {
      continue;
    }

    ZeroMem (&Range, sizeof (Range));
    Range.Hdr.Type                = SMBIOS_TYPE_MEMORY_ARRAY_MAPPED_ADDRESS;
    Range.Hdr.Length              = sizeof (Range);
    Range.MemoryArrayHandle       = ArrayHandle;
    Range.PartitionWidth          = (UINT8)DeviceCount;
    Range.StartingAddress         = Range.EndingAddress = MAX_UINT32;
    Range.ExtendedStartingAddress = Resource->PhysicalStart;
    Range.ExtendedEndingAddress   = Resource->PhysicalStart + Resource->ResourceLength - 1;
    Status                        = AddRecord (Smbios, &Range.Hdr, NULL, 0, &Handle);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  DEBUG ((
    DEBUG_INFO,
    "K3 SMBIOS: %a, %lu MiB installed RAM\n",
    SystemStrings[1],
    Total / SIZE_1MB
    ));
  return EFI_SUCCESS;
}
