/** @file PciHostBridgeLibConstructor.c

  Copyright (c) 2023, SOPHGO Inc. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/CpuMmioAddressLib.h>
#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include "Cadence.h"
#include "Pioneer.h"
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/PlatformPciController.h>
#include <Protocol/PciHostBridgeResourceAllocation.h>
#include <Protocol/PciIo.h>
#include <IndustryStandard/Pci22.h>
#include <Library/UefiBootServicesTableLib.h>

#define RP_NO_BAR  2

#define UPPER_32_BITS(n)      ((UINT32)((n) >> 32))
#define LOWER_32_BITS(n)      ((UINT32)((n) & 0xffffffff))

STATIC
VOID
PcieHostInitRootPort (
  IN UINT32                   VendorId,
  IN UINT32                   DeviceId,
  IN UINT64                   CfgBase
  )
{
  INT32    Ctrl;
  UINT32   Id;
  UINT32   Value;

  //
  // Set the root complex BAR configuration register:
  // - disable both BAR0 and BAR1.
  // - enable Prefetchable Memory Base and Limit registers in type 1
  //   config space (64 bits).
  // - enable IO Base and Limit registers in type 1 config
  //   space (32 bits).
  //
  Ctrl = PCIE_LM_BAR_CFG_CTRL_DISABLED;
  Value = PCIE_LM_RC_BAR_CFG_BAR0_CTRL(Ctrl) |
    PCIE_LM_RC_BAR_CFG_BAR1_CTRL(Ctrl) |
    PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_ENABLE |
    PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_64BITS |
    PCIE_LM_RC_BAR_CFG_IO_ENABLE |
    PCIE_LM_RC_BAR_CFG_IO_32BITS;
  MmioWrite32 ((UINTN)(CfgBase + PCIE_LM_RC_BAR_CFG), Value);

  //
  // Set root port configuration space
  //
  if (VendorId != 0xffff) {
    Id = PCIE_LM_ID_VENDOR(VendorId) | PCIE_LM_ID_SUBSYS(VendorId);
    MmioWrite32((UINTN)(CfgBase + PCIE_LM_ID), Id);
  }

  if (DeviceId != 0xffff) {
    Value = MmioRead32 ((UINTN)(CfgBase + PCIE_RP_BASE + PCI_VENDOR_ID_OFFSET));
    Value &= 0x0000FFFF;
    Value |= (DeviceId << 16);
    MmioWrite32 ((UINTN)(CfgBase + PCIE_RP_BASE + PCI_VENDOR_ID_OFFSET), Value);
  }
  MmioWrite32 ((UINTN)(CfgBase + PCIE_RP_BASE + PCI_REVISION_ID_OFFSET), PCI_CLASS_BRIDGE_PCI << 16);
}

STATIC
VOID
PcieHostNoBarMatchInboundConfig (
  IN UINT32                   NoBarNbits,
  IN UINT64                   CfgBase
  )
{
  UINT32        Addr0;
  UINT32        Addr1;

  //
  // Set Root Port no BAR match Inbound Translation registers:
  // needed for MSI and DMA.
  // Root Port BAR0 and BAR1 are disabled, hence no need to set
  // their inbound translation registers.
  //
  Addr0 = PCIE_AT_IB_RP_BAR_ADDR0_NBITS (NoBarNbits);
  Addr1 = 0;
  MmioWrite32 ((UINTN)(CfgBase + PCIE_AT_IB_RP_BAR_ADDR0(RP_NO_BAR)), Addr0);
  MmioWrite32 ((UINTN)(CfgBase + PCIE_AT_IB_RP_BAR_ADDR1(RP_NO_BAR)), Addr1);
}

STATIC
VOID
PcieHostSetOutboundRegionForConfigureSpaceAccess (
  IN UINT64                   CfgBase,
  IN UINT64                   SlvAddr,
  IN INT32                    BusNumber
  )
{
  UINT32        Addr0;
  UINT32        Addr1;
  UINT32        Desc1;

  //
  // Set the PCI Address for Region 0
  // Reserve region 0 for PCI configure space accesses:
  // OB_REGION_PCI_ADDR0 and OB_REGION_DESC0 are updated dynamically by
  // PciMapBus (), other region registers are set here once for all.
  //
  Addr1 = 0; // Should be programmed to zero.
  Desc1 = PCIE_AT_OB_REGION_DESC1_BUS(BusNumber);
  MmioWrite32 ((UINTN)(CfgBase + PCIE_AT_OB_REGION_PCI_ADDR1(0)), Addr1);
  MmioWrite32 ((UINTN)(CfgBase + PCIE_AT_OB_REGION_DESC1(0)), Desc1);

  //
  // Set the AXI Address for Region 0
  //
  Addr0 = PCIE_AT_OB_REGION_CPU_ADDR0_NBITS(12) |
          (LOWER_32_BITS(SlvAddr) & GENMASK(31, 8));
  Addr1 = UPPER_32_BITS(SlvAddr);
  MmioWrite32 ((UINTN)(CfgBase + PCIE_AT_OB_REGION_CPU_ADDR0(0)), Addr0);
  MmioWrite32 ((UINTN)(CfgBase + PCIE_AT_OB_REGION_CPU_ADDR1(0)), Addr1);
}

STATIC
VOID
PcieHostSetOutboundRegion (
  IN UINT64                   CfgBase,
  IN INT32                    BusNumber,
  IN INT32                    RegionNumber,
  IN BOOLEAN                  IsMemory,
  IN UINT64                   PciAddr,
  IN UINT64                   CpuAddr,
  IN UINT64                   RegionSize
  )
{
  UINT32        Addr0;
  UINT32        Addr1;
  UINT32        Desc0;
  UINT32        Desc1;
  UINT32        Nbits;

  //
  // Get shift amount
  //
  Nbits         = 0;
  RegionSize   -= 1;
  while (RegionSize >= 1) {
    RegionSize >>= 1;
    Nbits++;
  }

  if (Nbits < 8) {
    Nbits = 8;
  }

  //
  // Set the PCI address, RegionNumber >= 1
  //
  Addr0 = PCIE_AT_OB_REGION_PCI_ADDR0_NBITS(Nbits) |
          (LOWER_32_BITS(PciAddr) & GENMASK(31, 8));
  Addr1 = UPPER_32_BITS(PciAddr);

  MmioWrite32 ((UINTN)(CfgBase + PCIE_AT_OB_REGION_PCI_ADDR0(RegionNumber)), Addr0);
  MmioWrite32 ((UINTN)(CfgBase + PCIE_AT_OB_REGION_PCI_ADDR1(RegionNumber)), Addr1);

  //
  // Set the PCIe header descriptor
  //
  if (IsMemory) {
    Desc0 = PCIE_AT_OB_REGION_DESC0_TYPE_MEM;
  } else {
    Desc0 = PCIE_AT_OB_REGION_DESC0_TYPE_IO;
  }
  Desc1 = 0;

  //
  // In Root Complex mode, the function number is always 0.
  //
  Desc0 |= PCIE_AT_OB_REGION_DESC0_HARDCODED_RID |
           PCIE_AT_OB_REGION_DESC0_DEVFN(0);
  Desc1 |= PCIE_AT_OB_REGION_DESC1_BUS(BusNumber);
  MmioWrite32 ((UINTN)(CfgBase + PCIE_AT_OB_REGION_DESC0(RegionNumber)), Desc0);
  MmioWrite32 ((UINTN)(CfgBase + PCIE_AT_OB_REGION_DESC1(RegionNumber)), Desc1);

  //
  // Set the CPU address
  //
  Addr0 = PCIE_AT_OB_REGION_CPU_ADDR0_NBITS(Nbits) |
          (LOWER_32_BITS(CpuAddr) & GENMASK(31, 8));
  Addr1 = UPPER_32_BITS(CpuAddr);
  MmioWrite32 ((UINTN)(CfgBase + PCIE_AT_OB_REGION_CPU_ADDR0(RegionNumber)), Addr0);
  MmioWrite32 ((UINTN)(CfgBase + PCIE_AT_OB_REGION_CPU_ADDR1(RegionNumber)), Addr1);
}

#define MAX_ROOTS  4
typedef struct {
  UINTN Reg;
  UINTN Cfg;
  struct {
    ACPI_HID_DEVICE_PATH Hid;
    EFI_DEVICE_PATH_PROTOCOL End;
  } Path;
} SG_PORT;

STATIC SG_PORT mPorts[MAX_ROOTS];
STATIC UINT64 mIoBases[MAX_ROOTS];
STATIC PCI_ROOT_BRIDGE mRoots[MAX_ROOTS];
STATIC RUYI_MULTI_ROOT_PCI_CONTROLLER mHost;

STATIC EFI_STATUS EFIAPI
Config (UINT16 Segment, UINT8 Bus, UINT8 Device, UINT8 Function, UINT16 Reg,
        UINTN Width, BOOLEAN Write, UINT32 *Value)
{
  UINTN Index, Base, Address;
  UINT32 Desc, Shift, Mask, Word;
  EFI_TPL Old;
  for (Index = 0; Index < mHost.Count; Index++) {
    if (mRoots[Index].Segment == Segment) { break; }
  }
  if (Index == mHost.Count || Device >= 32 || Function >= 8 ||
      (Width != 1 && Width != 2 && Width != 4) || Reg >= 4096 || (Reg & (Width - 1)) || !Value) {
    return EFI_INVALID_PARAMETER;
  }
  Base = mPorts[Index].Reg;
  if ((!Bus && (Device || Function)) || (Bus == 1 && Device) ||
      (Bus && !(MmioRead32 (Base + PCIE_LM_BASE) & BIT0))) {
    return EFI_NOT_FOUND;
  }
  Old = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  if (!Bus) {
    // The root port accepts only aligned 32-bit accesses.
    Address = Base + PCIE_RP_BASE + (Reg & ~3U);
    Shift = (Reg & 3U) * 8;
    Mask = MAX_UINT32 >> ((4 - Width) * 8);
    Word = (Write && Width == 4) ? 0 : MmioRead32 (Address);
    if (Write) {
      MmioWrite32 (Address, (Word & ~(Mask << Shift)) | ((*Value & Mask) << Shift));
    } else {
      *Value = (Word >> Shift) & Mask;
    }
  } else {
    Desc = (Bus == 1) ? PCIE_AT_OB_REGION_DESC0_TYPE_CONF_TYPE0 : PCIE_AT_OB_REGION_DESC0_TYPE_CONF_TYPE1;
    MmioWrite32 (Base + PCIE_AT_LINKDOWN, 0);
    MmioWrite32 (Base + PCIE_AT_OB_REGION_PCI_ADDR0(0), PCIE_AT_OB_REGION_PCI_ADDR0_NBITS(12) |
                 PCIE_AT_OB_REGION_PCI_ADDR0_BUS(Bus) | PCIE_AT_OB_REGION_PCI_ADDR0_DEVFN((Device << 3) | Function));
    MmioWrite32 (Base + PCIE_AT_OB_REGION_DESC0(0), PCIE_AT_OB_REGION_DESC0_HARDCODED_RID | Desc);
    Address = CpuMmioAddress (mPorts[Index].Cfg) + Reg;
    if (Write) {
      if (Width == 1) { MmioWrite8 (Address, *Value); }
      else if (Width == 2) { MmioWrite16 (Address, *Value); }
      else { MmioWrite32 (Address, *Value); }
    } else {
      if (Width == 1) { *Value = MmioRead8 (Address); }
      else if (Width == 2) { *Value = MmioRead16 (Address); }
      else { *Value = MmioRead32 (Address); }
    }
  }
  gBS->RestoreTPL (Old);
  return EFI_SUCCESS;
}

STATIC UINT64
Cells (CONST UINT32 *P)
{
  return LShiftU64 (Fdt32ToCpu (P[0]), 32) | Fdt32ToCpu (P[1]);
}

STATIC EFI_STATUS
InitPort (CONST VOID *Fdt, INT32 Node, UINTN I)
{
  SG_PORT *Port = &mPorts[I];
  PCI_ROOT_BRIDGE *Root = &mRoots[I];
  CONST UINT32 *P, *Ranges;
  INT32 Len, Count;
  UINTN Domain, RegSize, CfgSize;
  EFI_STATUS Status;
  P = FdtGetProp (Fdt, Node, "bus-range", &Len);
  if (!P || Len != 8 || P[0] || Fdt32ToCpu (P[1]) != 255) { return EFI_UNSUPPORTED; }
  INT32 RegIndex = FdtStringListSearch (Fdt, Node, "reg-names", "reg");
  INT32 CfgIndex = FdtStringListSearch (Fdt, Node, "reg-names", "cfg");
  if (RegIndex < 0 || CfgIndex < 0) { return EFI_NOT_FOUND; }
  Status = FdtGetMmio (Fdt, Node, RegIndex, PCIE_AT_LINKDOWN + 4, &Port->Reg, &RegSize);
  if (EFI_ERROR (Status)) { return Status; }
  P = FdtGetProp (Fdt, Node, "linux,pci-domain", &Len);
  if (P == NULL) {
    INT32 PortIndex = Sg2042PciPortIndex (Port->Reg);
    if (PortIndex < 0) { return EFI_UNSUPPORTED; }
    Domain = (UINTN)PortIndex;
  } else {
    if (Len != sizeof (*P) || Fdt32ToCpu (*P) > MAX_UINT16) { return EFI_COMPROMISED_DATA; }
    Domain = Fdt32ToCpu (*P);
  }
  for (UINTN J = 0; J < I; J++) {
    if (mRoots[J].Segment == Domain) { return EFI_COMPROMISED_DATA; }
  }
  Status = FdtGetMmio (Fdt, Node, CfgIndex, EFI_PAGE_SIZE, &Port->Cfg, &CfgSize);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (Port->Reg, RegSize);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (Port->Cfg, CfgSize);
  if (EFI_ERROR (Status)) { return Status; }

  Port->Reg = CpuMmioAddress (Port->Reg);

  Root->Segment = Domain;
  Root->Bus.Limit = 255;
  Root->Io.Base = Root->Mem.Base = Root->PMem.Base = MAX_UINT64;
  Root->MemAbove4G.Base = Root->PMemAbove4G.Base = MAX_UINT64;
  Root->DmaAbove4G = TRUE;
  Root->Supports = EFI_PCI_IO_ATTRIBUTE_IO | EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER |
                   EFI_PCI_ATTRIBUTE_DUAL_ADDRESS_CYCLE;
  Root->Attributes = Root->Supports & ~EFI_PCI_ATTRIBUTE_DUAL_ADDRESS_CYCLE;
  Root->AllocationAttributes = EFI_PCI_HOST_BRIDGE_MEM64_DECODE;
  Port->Path.Hid.Header.Type = ACPI_DEVICE_PATH;
  Port->Path.Hid.Header.SubType = ACPI_DP;
  SetDevicePathNodeLength (&Port->Path.Hid.Header, sizeof (Port->Path.Hid));
  Port->Path.Hid.HID = EISA_PNP_ID (0x0a08);
  Port->Path.Hid.UID = (UINT32)Domain;
  SetDevicePathEndNode (&Port->Path.End);
  Root->DevicePath = &Port->Path.Hid.Header;

  Ranges = FdtGetProp (Fdt, Node, "ranges", &Len);
  if (!Ranges || Len <= 0 || Len % 28 || Len / 28 > 5) { return EFI_COMPROMISED_DATA; }
  Count = Len / 28;
  for (INT32 J = 0; J < Count; J++) {
    P = Ranges + J * 7;
    UINT32 Flags = Fdt32ToCpu (P[0]);
    UINT32 Space = (Flags >> 24) & 3;
    UINT64 Pci = Cells (P + 1), Cpu = Cells (P + 3), Size = Cells (P + 5);
    PCI_ROOT_BRIDGE_APERTURE *Aperture;
    if (!Space || Size < EFI_PAGE_SIZE || (Size & (Size - 1)) || ((Pci | Cpu) & (Size - 1)) ||
        Pci > MAX_UINT64 - Size || Cpu > MAX_UINT64 - Size) { return EFI_UNSUPPORTED; }
    if (Space == 1) { Aperture = &Root->Io; }
    else if (Pci <= MAX_UINT32) { Aperture = (Flags & BIT30) ? &Root->PMem : &Root->Mem; }
    else { Aperture = (Flags & BIT30) ? &Root->PMemAbove4G : &Root->MemAbove4G; }
    if (Aperture->Base != MAX_UINT64) { return EFI_COMPROMISED_DATA; }
    Aperture->Base = Pci;
    Aperture->Limit = Pci + Size - 1;
    // The CPU HOB exposes a 32-bit port I/O space. Map its port numbers
    // to the full physical MMIO aperture in the CPU I/O protocol.
    Aperture->Translation = Pci - ((Space == 1) ? (UINT32)Cpu : Cpu);
    if (Space == 1) {
      mIoBases[I] = Cpu;
      Status = FdtMapMmio (Cpu, Size);
      if (EFI_ERROR (Status)) { return Status; }
    }
    PcieHostSetOutboundRegion (Port->Reg, 0, J + 1, Space != 1, Pci, Cpu, Size);
  }
  PcieHostInitRootPort (0x1f1c, 0x2042, Port->Reg);
  PcieHostNoBarMatchInboundConfig (48, Port->Reg);
  PcieHostSetOutboundRegionForConfigureSpaceAccess (Port->Reg, Port->Cfg, 0);
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
Sg2042PciEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  VOID *Fdt;
  EFI_HANDLE Handle = NULL;
  INT32 Node = -1;
  EFI_STATUS Status = Sg2042PreparePciFdt (&Fdt);
  if (EFI_ERROR (Status)) { return Status; }
  while ((Node = FdtNodeOffsetByCompatible (Fdt, Node, "sophgo,sg2042-pcie-host")) >= 0) {
    if (!FdtNodeEnabled (Fdt, Node)) { continue; }
    if (mHost.Count == MAX_ROOTS) { return EFI_COMPROMISED_DATA; }
    Status = InitPort (Fdt, Node, mHost.Count);
    if (EFI_ERROR (Status)) { return Status; }
    mHost.Count++;
  }
  if (!mHost.Count) { return EFI_NOT_FOUND; }
  mHost.Controller.Revision = 2;
  mHost.Controller.Config = Config;
  mHost.Bridges = mRoots;
  mHost.IoMmioBases = mIoBases;
  return gBS->InstallProtocolInterface (&Handle, &gRuyiPciControllerProtocolGuid,
                                         EFI_NATIVE_INTERFACE, &mHost);
}
