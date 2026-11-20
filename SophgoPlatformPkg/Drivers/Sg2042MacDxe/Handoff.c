/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/CpuMmioAddressLib.h>
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Guid/Fdt.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/Sg2042MacLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC BOOLEAN
ReadProperty (VOID *Fdt, INT32 Node, CONST CHAR8 *Name, UINT8 Mac[6])
{
  INT32 Size;
  CONST UINT8 *Prop = FdtGetProp (Fdt, Node, Name, &Size);
  if (Size != 6 || !Sg2042ValidMac (Prop)) { return FALSE; }
  CopyMem (Mac, Prop, 6);
  return TRUE;
}

STATIC EFI_STATUS
SetAddress (VOID *Fdt, INT32 Node, CONST UINT8 Mac[6])
{
  if (FdtSetProp (Fdt, Node, "mac-address", Mac, 6) ||
      FdtSetProp (Fdt, Node, "local-mac-address", Mac, 6)) { return EFI_OUT_OF_RESOURCES; }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
GmacAddress (VOID *Fdt, INT32 Node)
{
  UINT8 Mac[6];
  UINTN Base, Gate, Size;
  INT32 Clk = FdtNodeOffsetByCompatible (Fdt, -1, "sophgo,sg2042-clkgen");
  EFI_STATUS Status = FdtGetMmio (Fdt, Node, 0, 0x308, &Base, &Size);
  if (EFI_ERROR (Status)) { return Status; }
  if (ReadProperty (Fdt, Node, "mac-address", Mac) || ReadProperty (Fdt, Node, "local-mac-address", Mac)) {
    return SetAddress (Fdt, Node, Mac);
  }
  Status = FdtMapMmio (Base, Size);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtGetMmio (Fdt, Clk, 0, sizeof (UINT32), &Gate, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (Gate, sizeof (UINT32));
  if (EFI_ERROR (Status)) { return Status; }

  // MAC address slot zero on the DWMAC. Only its AXI gate is needed to read it.
  UINTN Port = Base;
  Base = CpuMmioAddress (Base);
  Gate = CpuMmioAddress (Gate);
  UINT32 Saved = MmioRead32 (Gate);
  MmioOr32 (Gate, BIT31);
  UINT32 Low = MmioRead32 (Base + 0x304);
  UINT16 High = (UINT16)MmioRead32 (Base + 0x300);
  if (!(Saved & BIT31)) { MmioAnd32 (Gate, ~BIT31); }
  CopyMem (Mac, &Low, sizeof (Low));
  CopyMem (Mac + 4, &High, sizeof (High));
  if (!Sg2042ValidMac (Mac)) {
    Status = Sg2042FallbackMac (&Port, sizeof (Port), TRUE, Mac);
    if (EFI_ERROR (Status)) { return Status; }
  }
  DEBUG ((DEBUG_INFO, "SG2042 GMAC %lx MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
          (UINT64)Port, Mac[0], Mac[1], Mac[2], Mac[3], Mac[4], Mac[5]));
  return SetAddress (Fdt, Node, Mac);
}

STATIC INT32
PciChild (VOID *Fdt, INT32 Parent, PCI_DEVICE_PATH *Pci, BOOLEAN Bridge)
{
  UINT32 DevFn = (Pci->Device << 3) | Pci->Function;
  UINT32 Reg[5] = { CpuToFdt32 (DevFn << 8), 0, 0, 0, 0 };
  INT32 Node, Size;
  FdtForEachSubnode (Node, Fdt, Parent) {
    CONST UINT32 *Address = FdtGetProp (Fdt, Node, "reg", &Size);
    if (Address && Size >= (INT32)sizeof (Reg) && ((Fdt32ToCpu (*Address) >> 8) & 0xff) == DevFn) { return Node; }
  }

  CHAR8 Name[32];
  AsciiSPrint (Name, sizeof (Name), "%a@%x,%x", Bridge ? "pci" : "ethernet", Pci->Device, Pci->Function);
  Node = FdtAddSubnode (Fdt, Parent, Name);
  if (Node < 0 || FdtSetProp (Fdt, Node, "reg", Reg, sizeof (Reg))) { return -1; }
  if (Bridge) {
    UINT32 Cells = CpuToFdt32 (3);
    if (FdtSetPropString (Fdt, Node, "device_type", "pci") ||
        FdtSetProp (Fdt, Node, "#address-cells", &Cells, sizeof (Cells))) { return -1; }
    Cells = CpuToFdt32 (2);
    if (FdtSetProp (Fdt, Node, "#size-cells", &Cells, sizeof (Cells))) { return -1; }
  }
  return Node;
}

STATIC EFI_STATUS
PublishPci (VOID *Fdt, EFI_HANDLE Controller, EFI_PCI_IO_PROTOCOL *Pci, CONST UINT8 Mac[6])
{
  UINTN Segment, Bus, Device, Function;
  EFI_STATUS Status = Pci->GetLocation (Pci, &Segment, &Bus, &Device, &Function);
  if (EFI_ERROR (Status)) { return Status; }
  INT32 Node = -1, Len;
  while ((Node = FdtNodeOffsetByCompatible (Fdt, Node, "sophgo,sg2042-pcie-host")) >= 0) {
    CONST UINT32 *Domain = FdtGetProp (Fdt, Node, "linux,pci-domain", &Len);
    if (Domain && Len == sizeof (*Domain) && Fdt32ToCpu (*Domain) == Segment) { break; }
  }
  if (Node < 0) { return EFI_NOT_FOUND; }
  EFI_DEVICE_PATH_PROTOCOL *Path = DevicePathFromHandle (Controller);
  if (!Path) { return EFI_NOT_FOUND; }
  for (; !IsDevicePathEnd (Path); Path = NextDevicePathNode (Path)) {
    if (DevicePathType (Path) != HARDWARE_DEVICE_PATH || DevicePathSubType (Path) != HW_PCI_DP) { continue; }
    if (DevicePathNodeLength (Path) != sizeof (PCI_DEVICE_PATH)) { return EFI_COMPROMISED_DATA; }
    BOOLEAN Bridge = !IsDevicePathEnd (NextDevicePathNode (Path));
    Node = PciChild (Fdt, Node, (PCI_DEVICE_PATH *)Path, Bridge);
    if (Node < 0) { return EFI_OUT_OF_RESOURCES; }
    if (!Bridge) { return SetAddress (Fdt, Node, Mac); }
  }
  return EFI_NOT_FOUND;
}

STATIC VOID EFIAPI
CheckAddresses (EFI_EVENT Event, VOID *Context)
{
  VOID *Original;
  EFI_STATUS Status;
  EFI_PHYSICAL_ADDRESS Address;
  EFI_HANDLE *Handles;
  VOID *Work;
  UINTN Count;
  BOOLEAN Changed = FALSE;

  gBS->CloseEvent (Event);
  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Original);
  if (EFI_ERROR (Status) || FdtCheckHeader (Original) || FdtTotalSize (Original) > SIZE_1MB) { return; }
  UINTN Pages = EFI_SIZE_TO_PAGES (FdtTotalSize (Original) + EFI_PAGE_SIZE);
  Status = gBS->AllocatePages (AllocateAnyPages, EfiACPIReclaimMemory, Pages, &Address);
  if (EFI_ERROR (Status)) { return; }
  VOID *Fdt = (VOID *)(UINTN)Address;
  UINTN Capacity = EFI_PAGES_TO_SIZE (Pages);
  if (FdtOpenInto (Original, Fdt, Capacity)) { goto Done; }
  Status = gBS->AllocatePool (EfiBootServicesData, Capacity, &Work);
  if (EFI_ERROR (Status)) { goto Done; }

  // A port can add several nodes and properties. Keep only complete edits.

  INT32 Node = FdtNodeOffsetByCompatible (Fdt, -1, "sophgo,sg2042-dwmac");
  if (Node >= 0 && FdtNodeEnabled (Fdt, Node)) {
    CopyMem (Work, Fdt, Capacity);
    Status = GmacAddress (Work, Node);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "SG2042 GMAC address: %r\n", Status));
    } else {
      CopyMem (Fdt, Work, Capacity);
      Changed = TRUE;
    }
  }

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status)) {
    for (UINTN I = 0; I < Count; I++) {
      EFI_PCI_IO_PROTOCOL *Pci;
      UINT32 Id;
      UINT8 Mac[6];
      BOOLEAN Handoff = FALSE;
      if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiPciIoProtocolGuid, (VOID **)&Pci)) ||
          EFI_ERROR (Pci->Pci.Read (Pci, EfiPciIoWidthUint32, 0, 1, &Id)) || Id != 0x812510ec) { continue; }
      Status = Sg2042PciMac (Handles[I], Pci, TRUE, Mac, &Handoff);
      if (!EFI_ERROR (Status)) {
        DEBUG ((DEBUG_INFO, "SG2042 RTL8125 MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                Mac[0], Mac[1], Mac[2], Mac[3], Mac[4], Mac[5]));
        if (Handoff) {
          CopyMem (Work, Fdt, Capacity);
          Status = PublishPci (Work, Handles[I], Pci, Mac);
          if (!EFI_ERROR (Status)) {
            CopyMem (Fdt, Work, Capacity);
            Changed = TRUE;
          }
        }
      }
      if (EFI_ERROR (Status)) { DEBUG ((DEBUG_ERROR, "SG2042 RTL8125 address: %r\n", Status)); }
    }
    gBS->FreePool (Handles);
  }
  gBS->FreePool (Work);
  if (Changed && !EFI_ERROR (gBS->InstallConfigurationTable (&gFdtTableGuid, Fdt))) { return; }
Done:
  gBS->FreePages (Address, Pages);
}

EFI_STATUS EFIAPI
Sg2042MacEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_EVENT Event;
  return gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, CheckAddresses, NULL, &gEfiEventReadyToBootGuid, &Event);
}
