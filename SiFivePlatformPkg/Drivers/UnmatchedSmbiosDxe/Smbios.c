/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <IndustryStandard/SmBios.h>
#include <Protocol/Smbios.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/Fu740InfoLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
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

EFI_STATUS EFIAPI
PublishUnmatchedInfo (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_SMBIOS_PROTOCOL     *Smbios;
  EFI_SMBIOS_HANDLE       Handle, ArrayHandle;
  FU740_BOARD_INFO        Info;
  SMBIOS_TABLE_TYPE0      Bios   = { 0 };
  SMBIOS_TABLE_TYPE1      System = { 0 };
  SMBIOS_TABLE_TYPE4      Cpu    = { 0 };
  SMBIOS_TABLE_TYPE16     Array  = { 0 };
  SMBIOS_TABLE_TYPE17     Memory = { 0 };
  SMBIOS_TABLE_TYPE19     Range  = { 0 };
  EFI_HOB_GENERIC_HEADER  *Hob;
  CONST CHAR8             *BiosStrings[]   = { "openRuyi", "EDK II stable202608" };
  CONST CHAR8             *SystemStrings[] = { "SiFive", NULL };
  CONST CHAR8             *CpuStrings[]    = { "SoC", "SiFive", "SiFive FU740" };
  CONST CHAR8             *MemoryStrings[] = { "Onboard DDR4" };
  EFI_STATUS              Status;
  UINT64                  MiB;

  Status = Fu740GetBoardInfo (&Info);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&Smbios);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Bios.Hdr.Type                               = SMBIOS_TYPE_BIOS_INFORMATION;
  Bios.Hdr.Length                             = sizeof (Bios);
  Bios.Vendor                                 = 1;
  Bios.BiosVersion                            = 2;
  Bios.BiosSize                               = 0xff;
  Bios.ExtendedBiosSize.Size                  = 32;
  Bios.BIOSCharacteristicsExtensionBytes[1]   = BIT3;
  Bios.EmbeddedControllerFirmwareMajorRelease = 0xff;
  Bios.EmbeddedControllerFirmwareMinorRelease = 0xff;
  Status                                      = AddRecord (Smbios, &Bios.Hdr, BiosStrings, ARRAY_SIZE (BiosStrings), &Handle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  System.Hdr.Type     = SMBIOS_TYPE_SYSTEM_INFORMATION;
  System.Hdr.Length   = sizeof (System);
  System.Manufacturer = 1;
  System.ProductName  = 2;
  System.WakeUpType   = SystemWakeupTypeUnknown;
  SystemStrings[1]    = Info.Model;
  Status              = AddRecord (Smbios, &System.Hdr, SystemStrings, ARRAY_SIZE (SystemStrings), &Handle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Cpu.Hdr.Type              = SMBIOS_TYPE_PROCESSOR_INFORMATION;
  Cpu.Hdr.Length            = sizeof (Cpu);
  Cpu.Socket                = 1;
  Cpu.ProcessorManufacturer = 2;
  Cpu.ProcessorVersion      = 3;
  Cpu.ProcessorType         = CentralProcessor;
  Cpu.ProcessorFamily       = ProcessorFamilyIndicatorFamily2;
  Cpu.ProcessorFamily2      = ProcessorFamilyRiscVRV64;
  Cpu.CurrentSpeed          = (UINT16)MIN (Info.CpuRate / 1000000, MAX_UINT16);
  Cpu.Status                = 0x41;
  Cpu.CoreCount             = Cpu.ThreadCount = Cpu.EnabledCoreCount = (UINT8)Info.CoreCount;
  Cpu.CoreCount2            = Cpu.ThreadCount2 = Cpu.EnabledCoreCount2 = Cpu.ThreadEnabled = (UINT16)Info.CoreCount;
  Cpu.L1CacheHandle         = Cpu.L2CacheHandle = Cpu.L3CacheHandle = 0xffff;
  Status                    = AddRecord (Smbios, &Cpu.Hdr, CpuStrings, ARRAY_SIZE (CpuStrings), &Handle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Array.Hdr.Type                     = SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY;
  Array.Hdr.Length                   = sizeof (Array);
  Array.Location                     = MemoryArrayLocationSystemBoard;
  Array.Use                          = MemoryArrayUseSystemMemory;
  Array.MemoryErrorCorrection        = MemoryErrorCorrectionUnknown;
  Array.MemoryErrorInformationHandle = 0xfffe;
  if (Info.MemorySize / SIZE_1KB < 0x80000000ULL) {
    Array.MaximumCapacity = (UINT32)(Info.MemorySize / SIZE_1KB);
  } else {
    Array.MaximumCapacity         = 0x80000000;
    Array.ExtendedMaximumCapacity = Info.MemorySize;
  }

  Array.NumberOfMemoryDevices = 1;
  Status                      = AddRecord (Smbios, &Array.Hdr, NULL, 0, &ArrayHandle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MiB = Info.MemorySize / SIZE_1MB;
  if (MiB > 0x7fffffff) {
    return EFI_UNSUPPORTED;
  }

  Memory.Hdr.Type                                          = SMBIOS_TYPE_MEMORY_DEVICE;
  Memory.Hdr.Length                                        = sizeof (Memory);
  Memory.MemoryArrayHandle                                 = ArrayHandle;
  Memory.MemoryErrorInformationHandle                      = 0xfffe;
  Memory.TotalWidth                                        = Memory.DataWidth = 64;
  Memory.FormFactor                                        = MemoryFormFactorRowOfChips;
  Memory.MemoryType                                        = MemoryTypeDdr4;
  Memory.MemoryTechnology                                  = MemoryTechnologyDram;
  Memory.MemoryOperatingModeCapability.Bits.VolatileMemory = 1;
  Memory.VolatileSize                                      = Info.MemorySize;
  Memory.DeviceLocator                                     = 1;
  Memory.Size                                              = MiB < 0x7fff ? (UINT16)MiB : 0x7fff;
  if (Memory.Size == 0x7fff) {
    Memory.ExtendedSize = (UINT32)MiB;
  }

  Status = AddRecord (Smbios, &Memory.Hdr, MemoryStrings, ARRAY_SIZE (MemoryStrings), &Handle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Hob = GetHobList (); !END_OF_HOB_LIST (Hob); Hob = GET_NEXT_HOB (Hob)) {
    EFI_HOB_RESOURCE_DESCRIPTOR  *Resource;
    if (Hob->HobType != EFI_HOB_TYPE_RESOURCE_DESCRIPTOR) {
      continue;
    }

    Resource = (VOID *)Hob;
    if (Resource->ResourceType != EFI_RESOURCE_SYSTEM_MEMORY) {
      continue;
    }

    if (!Resource->ResourceLength || (Resource->PhysicalStart > MAX_UINT64 - Resource->ResourceLength)) {
      return EFI_COMPROMISED_DATA;
    }

    Range.Hdr.Type                = SMBIOS_TYPE_MEMORY_ARRAY_MAPPED_ADDRESS;
    Range.Hdr.Length              = sizeof (Range);
    Range.MemoryArrayHandle       = ArrayHandle;
    Range.PartitionWidth          = 1;
    Range.StartingAddress         = Range.EndingAddress = MAX_UINT32;
    Range.ExtendedStartingAddress = Resource->PhysicalStart;
    Range.ExtendedEndingAddress   = Resource->PhysicalStart + Resource->ResourceLength - 1;
    Status                        = AddRecord (Smbios, &Range.Hdr, NULL, 0, &Handle);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}
