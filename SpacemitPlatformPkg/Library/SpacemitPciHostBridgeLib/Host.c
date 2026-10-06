/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <IndustryStandard/Pci.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PciHostBridgeLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Protocol/PciController.h>
#include <Protocol/PciRootBridgeIo.h>
#include <Protocol/PciIo.h>
#include <Protocol/PciHostBridgeResourceAllocation.h>

STATIC VOID
Unavailable (
  PCI_ROOT_BRIDGE_APERTURE  *Aperture
  )
{
  Aperture->Base        = MAX_UINT64;
  Aperture->Limit       = 0;
  Aperture->Translation = 0;
}

PCI_ROOT_BRIDGE *EFIAPI
PciHostBridgeGetRootBridges (
  UINTN  *Count
  )
{
  SPACEMIT_PCI_CONTROLLER_PROTOCOL  *Controller;
  PCI_ROOT_BRIDGE                   *Bridges;
  EFI_STATUS                        Status;

  if (Count == NULL) {
    return NULL;
  }

  *Count = 0;
  Status = gBS->LocateProtocol (&gSpacemitPciControllerProtocolGuid, NULL, (VOID **)&Controller);
  if (EFI_ERROR (Status) || !Controller || (Controller->Revision != 1) ||
      !Controller->Count || (Controller->Count > SPACEMIT_PCIE_MAX_PORTS) || !Controller->Ports)
  {
    return NULL;
  }

  Bridges = AllocateZeroPool (Controller->Count * sizeof (*Bridges));
  if (Bridges == NULL) {
    return NULL;
  }

  for (UINTN I = 0; I < Controller->Count; I++) {
    SPACEMIT_PCIE_PORT  *Port   = &Controller->Ports[I];
    PCI_ROOT_BRIDGE     *Bridge = &Bridges[I];
    if (!Port->Ready) {
      goto Fail;
    }

    Bridge->Segment  = Port->Segment;
    Bridge->Supports = EFI_PCI_IO_ATTRIBUTE_IO | EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER |
                       EFI_PCI_ATTRIBUTE_DUAL_ADDRESS_CYCLE;
    Bridge->Attributes = EFI_PCI_IO_ATTRIBUTE_IO | EFI_PCI_IO_ATTRIBUTE_MEMORY |
                         EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
    Bridge->DmaAbove4G            = TRUE;
    Bridge->NoExtendedConfigSpace = FALSE;
    Bridge->AllocationAttributes  = EFI_PCI_HOST_BRIDGE_MEM64_DECODE;
    Bridge->Bus.Base              = Port->BusMin;
    Bridge->Bus.Limit             = Port->BusMax;
    Unavailable (&Bridge->Io);
    Unavailable (&Bridge->Mem);
    Unavailable (&Bridge->MemAbove4G);
    Unavailable (&Bridge->PMem);
    Unavailable (&Bridge->PMemAbove4G);
    for (UINT32 R = 0; R < Port->RangeCount; R++) {
      SPACEMIT_PCIE_RANGE       *Range = &Port->Ranges[R];
      UINT32                    Type   = (Range->Flags >> 24) & 3;
      PCI_ROOT_BRIDGE_APERTURE  *Aperture;
      if (Type == 1) {
        Aperture = &Bridge->Io;
      } else if (Range->Flags & BIT30) {
        Aperture = Range->Pci >= SIZE_4GB ? &Bridge->PMemAbove4G : &Bridge->PMem;
      } else {
        Aperture = Range->Pci >= SIZE_4GB ? &Bridge->MemAbove4G : &Bridge->Mem;
      }

      Aperture->Base  = Range->Pci;
      Aperture->Limit = Range->Pci + Range->Size - 1;
      // Standard UEFI aperture translation is DEVICE minus HOST, including
      // wrap-around when the host aperture is above the PCI bus aperture.
      Aperture->Translation = Range->Pci - Range->Cpu;
    }

    ACPI_HID_DEVICE_PATH  Node = {
      { ACPI_DEVICE_PATH, ACPI_DP, { sizeof (ACPI_HID_DEVICE_PATH), 0 }
      },
      EISA_PNP_ID (0x0A08), Port->Segment
    };
    Bridge->DevicePath = AppendDevicePathNode (NULL, &Node.Header);
    if (Bridge->DevicePath == NULL) {
      goto Fail;
    }
  }

  *Count = Controller->Count;
  return Bridges;
Fail:
  PciHostBridgeFreeRootBridges (Bridges, Controller->Count);
  return NULL;
}

VOID EFIAPI
PciHostBridgeFreeRootBridges (
  PCI_ROOT_BRIDGE  *Bridges,
  UINTN            Count
  )
{
  if (Bridges == NULL) {
    return;
  }

  for (UINTN I = 0; I < Count; I++) {
    if (Bridges[I].DevicePath) {
      FreePool (Bridges[I].DevicePath);
    }
  }

  FreePool (Bridges);
}

VOID EFIAPI
PciHostBridgeResourceConflict (
  EFI_HANDLE  Host,
  VOID        *Configuration
  )
{
  DEBUG ((DEBUG_ERROR, "K3 PCI resource conflict; host=%p descriptors=%p\n", Host, Configuration));
}
