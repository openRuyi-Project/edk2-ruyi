/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Info.h"
#include <Guid/RiscVSecHobData.h>
#include <IndustryStandard/Pci.h>
#include <IndustryStandard/SmBios.h>
#include <Protocol/BlockIo.h>
#include <Protocol/Clock.h>
#include <Protocol/ComponentName2.h>
#include <Protocol/DiskInfo.h>
#include <Protocol/PciIo.h>
#include <Protocol/NonDiscoverableDevice.h>
#include <Protocol/Smbios.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define EMIT(Label, Value)  do {\
  Status = Emit (Context, (Label), (Value)); \
  if (EFI_ERROR (Status)) { return Status; } \
} while (0)

STATIC VOID
SizeString (
  UINT64  Bytes,
  CHAR16  *Text,
  UINTN   Size
  )
{
  UnicodeSPrint (
    Text,
    Size,
    L"%Lu.%02Lu GiB",
    Bytes / SIZE_1GB,
    ((Bytes % SIZE_1GB) * 100) / SIZE_1GB
    );
}

STATIC BOOLEAN
NodeEnabled (
  CONST VOID  *Fdt,
  INT32       Node
  )
{
  INT32        Length;
  CONST CHAR8  *Value = FdtGetProp (Fdt, Node, "status", &Length);

  return !Value || ((Length == 5) && !CompareMem (Value, "okay", 5)) ||
         ((Length == 3) && !CompareMem (Value, "ok", 3));
}

STATIC EFI_STATUS
CpuInfo (
  INFO_EMIT  Emit,
  VOID       *Context
  )
{
  CONST EFI_GUID           Guid = RISCV_SEC_HANDOFF_HOB_GUID;
  EFI_HOB_GUID_TYPE        *Hob = GetFirstGuidHob (&Guid);
  RISCV_SEC_HANDOFF_DATA   Handoff;
  SPACEMIT_CLOCK_PROTOCOL  *Clock;
  CONST CHAR8              *Value;
  CHAR16                   Text[128], Label[48];
  UINT32                   X100 = 0, A100 = 0, Other = 0;
  UINT64                   Rate;
  INT32                    Cpus, Node, Length;
  EFI_STATUS               Status;

  UnicodeSPrint (Text, sizeof (Text), L"%a", (CHAR8 *)FixedPcdGetPtr (PcdSystemProductName));
  EMIT (L"System", Text);
  EMIT (L"Processor", L"SpacemiT K3");
  if (!Hob || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (Handoff))) {
    return EFI_NOT_FOUND;
  }

  CopyMem (&Handoff, GET_GUID_HOB_DATA (Hob), sizeof (Handoff));
  if (!Handoff.FdtPointer || FdtCheckHeader (Handoff.FdtPointer)) {
    return EFI_COMPROMISED_DATA;
  }

  Cpus = FdtPathOffset (Handoff.FdtPointer, "/cpus");
  if (Cpus < 0) {
    return EFI_NOT_FOUND;
  }

  // Report this boot's active CPUs, not the setting saved for the next boot.
  FdtForEachSubnode (Node, Handoff.FdtPointer, Cpus) {
    Value = FdtGetProp (Handoff.FdtPointer, Node, "device_type", &Length);
    if (!Value || (Length != 4) || CompareMem (Value, "cpu", 4) ||
        !NodeEnabled (Handoff.FdtPointer, Node))
    {
      continue;
    }

    if (FdtStringListSearch (Handoff.FdtPointer, Node, "compatible", "spacemit,x100") >= 0) {
      X100++;
    } else if (FdtStringListSearch (Handoff.FdtPointer, Node, "compatible", "spacemit,a100") >= 0) {
      A100++;
    } else {
      Other++;
    }
  }

  UnicodeSPrint (Text, sizeof (Text), L"%u X100 + %u A100", X100, A100);
  EMIT (L"Enabled cores", Text);
  if (Other) {
    UnicodeSPrint (Text, sizeof (Text), L"%u", Other);
    EMIT (L"Other enabled cores", Text);
  }

  UnicodeSPrint (Text, sizeof (Text), L"%Lu", Handoff.BootHartId);
  EMIT (L"Boot hart", Text);
  Clock = NULL;
  gBS->LocateProtocol (&gSpacemitClockProtocolGuid, NULL, (VOID **)&Clock);
  if (X100) {
    for (UINT32 Cluster = 0; Cluster < 2; Cluster++) {
      UnicodeSPrint (Label, sizeof (Label), L"X100 cluster %u clock", Cluster);
      if (Clock && (Clock->Revision == 1) && Clock->GetRate &&
          !EFI_ERROR (Clock->GetRate (2 + Cluster, &Rate)) && Rate)
      {
        UnicodeSPrint (Text, sizeof (Text), L"%Lu MHz", Rate / 1000000);
        EMIT (Label, Text);
      } else {
        EMIT (Label, L"Unknown");
      }
    }
  }

  if (A100) {
    EMIT (L"A100 clock", L"Unknown");
  }

  return EFI_SUCCESS;
}

STATIC VOID
MemoryPart (
  CONST SMBIOS_TABLE_TYPE17  *Device,
  CHAR16                     *Text,
  UINTN                      Size
  )
{
  CONST CHAR8  *String = (CONST CHAR8 *)Device + Device->Hdr.Length;
  UINT8        Index   = Device->PartNumber;
  UINTN        Length  = 0;

  UnicodeSPrint (Text, Size, L"Unknown");
  if (!Index) {
    return;
  }

  while (--Index && *String) {
    String += AsciiStrSize (String);
  }

  if (!*String) {
    return;
  }

  while (*String && Length + 1 < Size / sizeof (*Text)) {
    Text[Length++] = (*String >= 0x20 && *String <= 0x7e) ? *String : L' ';
    String++;
  }

  Text[Length] = 0;
}

STATIC EFI_STATUS
MemoryInfo (
  INFO_EMIT  Emit,
  VOID       *Context
  )
{
  EFI_HOB_GENERIC_HEADER       *Hob;
  EFI_HOB_RESOURCE_DESCRIPTOR  *Resource;
  EFI_SMBIOS_PROTOCOL          *Smbios;
  EFI_SMBIOS_HANDLE            Handle = SMBIOS_HANDLE_PI_RESERVED;
  EFI_SMBIOS_TABLE_HEADER      *Record;
  EFI_SMBIOS_TYPE              Type = SMBIOS_TYPE_MEMORY_DEVICE;
  SMBIOS_TABLE_TYPE17          *Device;
  UINT64                       Total = 0, Bytes;
  UINTN                        Index = 0;
  CHAR16                       Text[128], Label[48];
  EFI_STATUS                   Status;

  for (Hob = GetHobList (); !END_OF_HOB_LIST (Hob); Hob = GET_NEXT_HOB (Hob)) {
    if (Hob->HobType != EFI_HOB_TYPE_RESOURCE_DESCRIPTOR) {
      continue;
    }

    Resource = (VOID *)Hob;
    if (Resource->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY) {
      if (Total > MAX_UINT64 - Resource->ResourceLength) {
        return EFI_COMPROMISED_DATA;
      }

      Total += Resource->ResourceLength;
    }
  }

  if (!Total) {
    return EFI_NOT_FOUND;
  }

  SizeString (Total, Text, sizeof (Text));
  EMIT (L"Installed memory", Text);
  Status = gBS->LocateProtocol (&gEfiSmbiosProtocolGuid, NULL, (VOID **)&Smbios);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  while (!EFI_ERROR (Smbios->GetNext (Smbios, &Handle, &Type, &Record, NULL))) {
    if (Record->Length < OFFSET_OF (SMBIOS_TABLE_TYPE17, Speed) + sizeof (Device->Speed)) {
      continue;
    }

    Device = (VOID *)Record;
    UnicodeSPrint (Label, sizeof (Label), L"Memory device %u", ++Index);
    if (Device->Size == 0xffff) {
      EMIT (Label, L"Unknown capacity");
    } else if (Device->Size == 0) {
      EMIT (Label, L"Not populated");
    } else {
      if (Device->Size == 0x7fff) {
        if (Record->Length < OFFSET_OF (SMBIOS_TABLE_TYPE17, ExtendedSize) + sizeof (Device->ExtendedSize)) {
          EMIT (Label, L"Unknown capacity");
          continue;
        }

        Bytes = (UINT64)(Device->ExtendedSize & 0x7fffffff) * SIZE_1MB;
      } else {
        Bytes = (UINT64)(Device->Size & 0x7fff) * ((Device->Size & BIT15) ? SIZE_1KB : SIZE_1MB);
      }

      SizeString (Bytes, Text, sizeof (Text));
      EMIT (Label, Text);
    }

    EMIT (
      L"  Type",
      Device->MemoryType == MemoryTypeLpddr5 ? L"LPDDR5" :
      Device->MemoryType == MemoryTypeLpddr4 ? L"LPDDR4" : L"Unknown"
      );
    if (Record->Length >= OFFSET_OF (SMBIOS_TABLE_TYPE17, PartNumber) + sizeof (Device->PartNumber)) {
      MemoryPart (Device, Text, sizeof (Text));
    } else {
      UnicodeSPrint (Text, sizeof (Text), L"Unknown");
    }

    EMIT (L"  Part number", Text);
    if ((Record->Length >= OFFSET_OF (SMBIOS_TABLE_TYPE17, ConfiguredMemoryClockSpeed) +
         sizeof (Device->ConfiguredMemoryClockSpeed)) && Device->ConfiguredMemoryClockSpeed &&
        (Device->ConfiguredMemoryClockSpeed != MAX_UINT16))
    {
      UnicodeSPrint (Text, sizeof (Text), L"%u MT/s", Device->ConfiguredMemoryClockSpeed);
      EMIT (L"  Configured data rate", Text);
    } else {
      EMIT (L"  Configured data rate", L"Unknown");
    }
  }

  return EFI_SUCCESS;
}

STATIC CONST CHAR16 *
PciClassName (
  UINT8  Class
  )
{
  switch (Class) {
    case PCI_CLASS_MASS_STORAGE: return L"Storage";
    case PCI_CLASS_NETWORK: return L"Network";
    case PCI_CLASS_DISPLAY: return L"Display";
    case PCI_CLASS_MEDIA: return L"Multimedia";
    case PCI_CLASS_BRIDGE: return L"Bridge";
    case PCI_CLASS_SERIAL: return L"Serial bus";
    default: return L"PCI device";
  }
}

STATIC VOID
LinkString (
  UINT16  Link,
  CHAR16  *Text,
  UINTN   Size
  )
{
  STATIC CONST CHAR16  *Rates[] = { L"Unknown", L"2.5", L"5", L"8", L"16", L"32", L"64" };
  UINT32               Speed = Link & 15, Width = (Link >> 4) & 63;

  if ((Speed == 0) || (Speed >= ARRAY_SIZE (Rates)) || (Width == 0)) {
    UnicodeSPrint (Text, Size, L"Unknown");
  } else {
    UnicodeSPrint (Text, Size, L"%s GT/s x%u", Rates[Speed], Width);
  }
}

// Conventional capability pointers are untrusted device data. Reject loops,
// misalignment and pointers which cannot contain the PCIe link registers.
STATIC UINT8
PcieCapability (
  CONST UINT8  *Config
  )
{
  BOOLEAN  Seen[64] = { FALSE };
  UINT8    Offset   = Config[PCI_CAPABILITY_POINTER_OFFSET];

  if (!(Config[PCI_PRIMARY_STATUS_OFFSET] & BIT4)) {
    return 0;
  }

  while (Offset) {
    if ((Offset < 0x40) || (Offset > 0xfc) || (Offset & 3) || Seen[Offset / 4]) {
      return 0;
    }

    Seen[Offset / 4] = TRUE;
    if (Config[Offset] == EFI_PCI_CAPABILITY_ID_PCIEXP) {
      if (Offset > 0xec) {
        return 0;
      }

      UINT8  PortType = (Config[Offset + 2] >> 4) & 15;
      // Root-complex integrated endpoints and event collectors have no link.
      return (PortType == 9 || PortType == 10) ? 0 : Offset;
    }

    Offset = Config[Offset + 1];
  }

  return 0;
}

typedef struct {
  EFI_PCI_IO_PROTOCOL    *Io;
  UINTN                  Segment, Bus, Device, Function;
} PCI_ENTRY;

STATIC BOOLEAN
PciAfter (
  CONST PCI_ENTRY  *A,
  CONST PCI_ENTRY  *B
  )
{
  if (A->Segment != B->Segment) {
    return A->Segment > B->Segment;
  }

  if (A->Bus != B->Bus) {
    return A->Bus > B->Bus;
  }

  if (A->Device != B->Device) {
    return A->Device > B->Device;
  }

  return A->Function > B->Function;
}

STATIC EFI_STATUS
PciInfo (
  INFO_EMIT  Emit,
  VOID       *Context
  )
{
  EFI_HANDLE  *Handles;
  PCI_ENTRY   *Entries, Entry;
  UINTN       Count, Used = 0;
  UINT8       Config[256], Cap;
  CHAR16      Label[80], Text[128], Link[48];
  EFI_STATUS  Status;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Count > MAX_UINTN / sizeof (*Entries)) {
    FreePool (Handles);
    return EFI_OUT_OF_RESOURCES;
  }

  Entries = AllocateZeroPool (Count * sizeof (*Entries));
  if (!Entries) {
    FreePool (Handles);
    return EFI_OUT_OF_RESOURCES;
  }

  for (UINTN I = 0; I < Count; I++) {
    if (!EFI_ERROR (
           gBS->OpenProtocol (
                  Handles[I],
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  NULL,
                  NULL,
                  NULL,
                  EFI_OPEN_PROTOCOL_TEST_PROTOCOL
                  )
           ))
    {
      continue;
    }

    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiPciIoProtocolGuid, (VOID **)&Entry.Io)) ||
        EFI_ERROR (Entry.Io->GetLocation (Entry.Io, &Entry.Segment, &Entry.Bus, &Entry.Device, &Entry.Function)))
    {
      continue;
    }

    // Exclude non-discoverable devices presented through a synthetic PCI bus.
    if ((Entry.Bus > 255) || (Entry.Device > 31) || (Entry.Function > 7)) {
      continue;
    }

    UINTN  J = Used++;
    while (J && PciAfter (&Entries[J - 1], &Entry)) {
      Entries[J] = Entries[J - 1];
      J--;
    }

    Entries[J] = Entry;
  }

  FreePool (Handles);
  for (UINTN I = 0; I < Used; I++) {
    Entry = Entries[I];
    UnicodeSPrint (Label, sizeof (Label), L"%04x:%02x:%02x.%x", Entry.Segment, Entry.Bus, Entry.Device, Entry.Function);
    Status = Entry.Io->Pci.Read (Entry.Io, EfiPciIoWidthUint8, 0, sizeof (Config), Config);
    if (EFI_ERROR (Status) || (ReadUnaligned16 ((CONST UINT16 *)Config) == 0xffff)) {
      Status = Emit (Context, Label, L"Unavailable");
    } else {
      UnicodeSPrint (
        Text,
        sizeof (Text),
        L"%s %04x:%04x",
        PciClassName (Config[11]),
        ReadUnaligned16 ((CONST UINT16 *)Config),
        ReadUnaligned16 ((CONST UINT16 *)(Config + 2))
        );
      Status = Emit (Context, Label, Text);
      Cap    = PcieCapability (Config);
      if (!EFI_ERROR (Status) && Cap) {
        LinkString (ReadUnaligned16 ((CONST UINT16 *)(Config + Cap + 0x12)), Link, sizeof (Link));
        Status = Emit (Context, L"  Negotiated link", Link);
        if (!EFI_ERROR (Status)) {
          LinkString (ReadUnaligned16 ((CONST UINT16 *)(Config + Cap + 0x0c)), Link, sizeof (Link));
          Status = Emit (Context, L"  Device maximum", Link);
        }
      }
    }

    if (EFI_ERROR (Status)) {
      break;
    }
  }

  FreePool (Entries);
  return Used ? Status : EFI_NOT_FOUND;
}

// Convert fixed-width identify fields without treating device bytes as format
// strings or allowing control characters into the setup console.
STATIC VOID
IdentifyString (
  CONST UINT8  *Data,
  UINTN        Length,
  BOOLEAN      Swap,
  CHAR16       *Text,
  UINTN        Capacity
  )
{
  UINTN  Used;

  if (!Capacity) {
    return;
  }

  if (Swap) {
    Length &= ~(UINTN)1;
  }

  Used = MIN (Length, Capacity - 1);

  for (UINTN I = 0; I < Used; I++) {
    UINT8  C = Data[Swap ? (I ^ 1) : I];
    Text[I] = (C >= 0x20 && C <= 0x7e) ? C : L' ';
  }

  while (Used && Text[Used - 1] == L' ') {
    Used--;
  }

  Text[Used] = 0;
}

// NVMe DiskInfo Identify returns namespace data, not the controller model.
// Retrieve the cached namespace name from its driver's ComponentName2 instead.
STATIC VOID
NvmeModel (
  EFI_HANDLE  Handle,
  CHAR16      *Text,
  UINTN       Size
  )
{
  EFI_DEVICE_PATH_PROTOCOL      *Path = DevicePathFromHandle (Handle);
  EFI_HANDLE                    Parent, *Drivers;
  EFI_COMPONENT_NAME2_PROTOCOL  *Name;
  CHAR16                        *Model;
  UINTN                         Count;

  if (!Path || EFI_ERROR (gBS->LocateDevicePath (&gEfiPciIoProtocolGuid, &Path, &Parent)) ||
      EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiComponentName2ProtocolGuid, NULL, &Count, &Drivers)))
  {
    return;
  }

  for (UINTN I = 0; I < Count; I++) {
    if (!EFI_ERROR (gBS->HandleProtocol (Drivers[I], &gEfiComponentName2ProtocolGuid, (VOID **)&Name)) &&
        !EFI_ERROR (Name->GetControllerName (Name, Parent, Handle, "en", &Model)) && Model)
    {
      UnicodeSPrint (Text, Size, L"%s", Model);
      break;
    }
  }

  FreePool (Drivers);
}

STATIC VOID
DiskModel (
  EFI_HANDLE  Handle,
  CHAR16      *Text,
  UINTN       Size
  )
{
  EFI_DISK_INFO_PROTOCOL  *Disk;
  UINT8                   Data[512];
  UINT32                  Length = sizeof (Data);
  EFI_STATUS              Status;

  UnicodeSPrint (Text, Size, L"Unknown");
  Status = gBS->HandleProtocol (Handle, &gEfiDiskInfoProtocolGuid, (VOID **)&Disk);
  if (EFI_ERROR (Status)) {
    return;
  }

  if (CompareGuid (&Disk->Interface, &gEfiDiskInfoNvmeInterfaceGuid)) {
    NvmeModel (Handle, Text, Size);
  } else if (CompareGuid (&Disk->Interface, &gEfiDiskInfoScsiInterfaceGuid) ||
             CompareGuid (&Disk->Interface, &gEfiDiskInfoUfsInterfaceGuid) ||
             CompareGuid (&Disk->Interface, &gEfiDiskInfoUsbInterfaceGuid))
  {
    Status = Disk->Inquiry (Disk, Data, &Length);
    if (!EFI_ERROR (Status) && (Length >= 32) && (Length <= sizeof (Data))) {
      CHAR16  Vendor[9], Product[17];
      IdentifyString (Data + 8, 8, FALSE, Vendor, ARRAY_SIZE (Vendor));
      IdentifyString (Data + 16, 16, FALSE, Product, ARRAY_SIZE (Product));
      UnicodeSPrint (Text, Size, L"%s %s", Vendor, Product);
    }
  } else if (CompareGuid (&Disk->Interface, &gEfiDiskInfoAhciInterfaceGuid) ||
             CompareGuid (&Disk->Interface, &gEfiDiskInfoIdeInterfaceGuid))
  {
    Status = Disk->Identify (Disk, Data, &Length);
    if (!EFI_ERROR (Status) && (Length == sizeof (Data))) {
      IdentifyString (Data + 54, 40, TRUE, Text, Size / sizeof (*Text));
    }
  }
}

STATIC CONST CHAR16 *
StorageType (
  EFI_DEVICE_PATH_PROTOCOL  *Path
  )
{
  for (EFI_DEVICE_PATH_PROTOCOL *Node = Path; Node && !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if ((DevicePathType (Node) == MEDIA_DEVICE_PATH) && (DevicePathSubType (Node) == MEDIA_RAM_DISK_DP)) {
      return L"RAM disk";
    }

    if (DevicePathType (Node) != MESSAGING_DEVICE_PATH) {
      continue;
    }

    switch (DevicePathSubType (Node)) {
      case MSG_UFS_DP: return L"UFS";
      case MSG_NVME_NAMESPACE_DP: return L"NVMe";
      case MSG_SATA_DP: return L"SATA";
      case MSG_USB_DP: return L"USB";
      case MSG_SD_DP: return L"SD";
      case MSG_EMMC_DP: return L"eMMC";
    }
  }

  return L"Block device";
}

STATIC BOOLEAN
StorageVisible (
  CONST EFI_BLOCK_IO_MEDIA  *Media,
  EFI_DEVICE_PATH_PROTOCOL  *Path
  )
{
  if (!Media || Media->LogicalPartition) {
    return FALSE;
  }

  for (EFI_DEVICE_PATH_PROTOCOL *Node = Path; Node && !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if ((DevicePathType (Node) == MESSAGING_DEVICE_PATH) &&
        (DevicePathSubType (Node) == MSG_UFS_DP) &&
        (DevicePathNodeLength (Node) == sizeof (UFS_DEVICE_PATH)) &&
        (((UFS_DEVICE_PATH *)Node)->Lun & 0x80))
    {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC BOOLEAN
DiskCapacity (
  CONST EFI_BLOCK_IO_MEDIA  *Media,
  UINT64                    *Bytes
  )
{
  if (!Media->BlockSize || (Media->LastBlock == MAX_UINT64) ||
      ((Media->LastBlock + 1) > MAX_UINT64 / Media->BlockSize))
  {
    return FALSE;
  }

  *Bytes = (Media->LastBlock + 1) * Media->BlockSize;
  return TRUE;
}

STATIC EFI_STATUS
StorageInfo (
  INFO_EMIT  Emit,
  VOID       *Context
  )
{
  EFI_HANDLE                *Handles;
  EFI_BLOCK_IO_PROTOCOL     *Io;
  EFI_DEVICE_PATH_PROTOCOL  *Path;
  UINTN                     Count, Used = 0;
  UINT64                    Bytes;
  CHAR16                    Label[80], Text[128];
  EFI_STATUS                Status;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (UINTN I = 0; I < Count; I++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiBlockIoProtocolGuid, (VOID **)&Io))) {
      continue;
    }

    Path = DevicePathFromHandle (Handles[I]);
    if (!StorageVisible (Io->Media, Path)) {
      continue;
    }

    UnicodeSPrint (Label, sizeof (Label), L"%u: %s", ++Used, StorageType (Path));
    DiskModel (Handles[I], Text, sizeof (Text));
    if (!Text[0]) {
      UnicodeSPrint (Text, sizeof (Text), L"Unknown");
    }

    Status = Emit (Context, Label, Text);
    if (EFI_ERROR (Status)) {
      break;
    }

    if (!Io->Media->MediaPresent) {
      Status = Emit (Context, L"  Media", L"Not present");
    } else {
      if (DiskCapacity (Io->Media, &Bytes)) {
        SizeString (Bytes, Text, sizeof (Text));
      } else {
        UnicodeSPrint (Text, sizeof (Text), L"Unknown");
      }

      Status = Emit (Context, L"  Capacity", Text);
      if (!EFI_ERROR (Status)) {
        UnicodeSPrint (
          Text,
          sizeof (Text),
          L"%u bytes; %s",
          Io->Media->BlockSize,
          Io->Media->ReadOnly ? L"read-only" : L"writable"
          );
        Status = Emit (Context, L"  Logical block", Text);
      }
    }

    if (EFI_ERROR (Status)) {
      break;
    }
  }

  FreePool (Handles);
  return Used ? Status : EFI_NOT_FOUND;
}

EFI_STATUS
CollectSystemInfo (
  UINT16     Form,
  INFO_EMIT  Emit,
  VOID       *Context
  )
{
  switch (Form) {
    case INFO_CPU: return CpuInfo (Emit, Context);
    case INFO_MEMORY: return MemoryInfo (Emit, Context);
    case INFO_PCI: return PciInfo (Emit, Context);
    case INFO_STORAGE: return StorageInfo (Emit, Context);
    default: return EFI_UNSUPPORTED;
  }
}
