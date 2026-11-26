/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * EIC7700 initialization follows Linux pcie-eswin.c.
 * Copyright 2026, Beijing ESWIN Computing Technology Co., Ltd.
 * Yu Ning, Senchuan Zhang, Yanghui Ou
 *
 * DWC configuration access follows the FU740 controller in this repository.
 * Copyright (C) 2019-2021 SiFive, Inc.
 * Paul Walmsley, Greentime Hu
 * Copyright (C) 2013 Kosagi
 */
#include <IndustryStandard/Eic7700ClockIds.h>
#include <IndustryStandard/Eic7702AddressMap.h>
#include <IndustryStandard/Eic7700ResetIds.h>
#include <Uefi.h>
#include <IndustryStandard/Pci.h>
#include <IndustryStandard/DwcPcieRegisters.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DwcPcieTrainingLib.h>
#include <Library/FdtBoardInfoLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/PlatformPciController.h>
#include <Protocol/PciHostBridgeResourceAllocation.h>
#include <Protocol/PciIo.h>

#define SYSCRG_TCU_ACLK        0x11c
#define SYSCRG_PCIE_ACLK       0x170
#define SYSCRG_PCIE_CFG        0x174
#define SYSCRG_PCIE_RESET      0x120
#define RESET_DBI              BIT0
#define RESET_POWER            BIT1
#define RESET_PERST            BIT2
#define CLOCK_GATE             BIT31
#define CLOCK_CR               BIT0
#define CLOCK_AUX              BIT1
#define PERST_PAD              (28 * sizeof (UINT32))
#define PAD_INPUT_ENABLE       BIT0
#define ELBI_CONTROL           0x000
#define ELBI_STATUS            0x100
#define ELBI_DEVICE_TYPE       0xfU
#define ELBI_ROOT_PORT         4U
#define ELBI_LTSSM_ENABLE      BIT5
#define ELBI_HOLD_PHY          BIT6
#define ELBI_PHY_AUX_CLOCK     BIT16
#define ELBI_LINK_UP           3U
#define ELBI_L0_STATUS         0x47U
#define DBI_VENDOR_DEVICE      0x20301fe1U
#define DBI_CS2_OFFSET         0x101000
#define DBI_MSI_CAP            0x050
#define DBI_EXP_CAP            0x070
#define DBI_LINK_CAP           (DBI_EXP_CAP + 0x0c)
#define DBI_PORT_LINK          0x710
#define DBI_LINK_MODE_MASK     (0x3fU << 16)
#define DBI_DLL_ENABLE         BIT5
#define DBI_FAST_LINK          BIT7
#define DBI_GEN2_CONTROL       0x80c
#define DBI_PREDETERMINED_LANES (0x1fU << 8)
#define DBI_MULTI_LANE         0x8c0
#define DBI_VERSION            0x8f8
#define ATU_OFFSET             0x300000
#define ATU_MEMORY             0U
#define ATU_IO                 2U
#define ATU_CONFIG0            4U
#define ATU_CONFIG1            5U
#define MAX_WINDOWS            4
#define DBI_MAP_SIZE           (ATU_OFFSET + (MAX_WINDOWS + 1) * DWC_ATU_REGION_STRIDE)

typedef struct {
  UINT64 Cpu, Pci, Size;
  UINT32 Type;
} PCI_WINDOW;

typedef struct {
  UINTN Dbi, Config, Elbi, Clock, Reset, Pins;
  UINT32 Lanes, MaxSpeed, SavedPad;
  UINT16 Segment;
  BOOLEAN Trained;
  PCI_WINDOW Windows[MAX_WINDOWS];
  UINTN WindowCount;
  struct {
    ACPI_HID_DEVICE_PATH Root;
    EFI_DEVICE_PATH_PROTOCOL End;
  } Path;
} EIC_PCIE_PORT;

#define MAX_ROOTS  2
STATIC EIC_PCIE_PORT mPorts[MAX_ROOTS];
STATIC PCI_ROOT_BRIDGE mRoots[MAX_ROOTS];
STATIC UINT64 mIoBases[MAX_ROOTS];
STATIC RUYI_MULTI_ROOT_PCI_CONTROLLER mHost;

STATIC UINT64
Cells64 (CONST UINT32 *Data)
{
  return LShiftU64 (Fdt32ToCpu (Data[0]), 32) | Fdt32ToCpu (Data[1]);
}

STATIC EFI_STATUS
Cell (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name, UINT32 *Value)
{
  INT32 Length;
  CONST UINT32 *Data = FdtGetProp (Fdt, Node, Name, &Length);
  if (!Data || Length != sizeof (*Data)) { return EFI_COMPROMISED_DATA; }
  *Value = Fdt32ToCpu (*Data);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
NamedMmio (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name,
           UINTN Minimum, UINTN *Base)
{
  INT32 Index = FdtStringListSearch (Fdt, Node, "reg-names", Name);
  EFI_STATUS Status;
  if (Index < 0) { return EFI_NOT_FOUND; }
  Status = FdtGetMmio (Fdt, Node, Index, Minimum, Base, NULL);
  return EFI_ERROR (Status) ? Status : FdtMapMmio (*Base, Minimum);
}

STATIC EFI_STATUS
Provider (CONST VOID *Fdt, UINT32 Phandle, CONST CHAR8 *Compatible,
          CONST CHAR8 *CellName, UINTN Minimum, UINTN *Base)
{
  INT32 Node = FdtNodeOffsetByPhandle (Fdt, Phandle);
  UINT32 Cells;
  EFI_STATUS Status;

  if (!FdtNodeEnabled (Fdt, Node) ||
      FdtStringListSearch (Fdt, Node, "compatible", Compatible) < 0 ||
      EFI_ERROR (Cell (Fdt, Node, CellName, &Cells)) || Cells != 1) {
    return EFI_UNSUPPORTED;
  }
  Status = FdtGetMmio (Fdt, Node, 0, Minimum, Base, NULL);
  return EFI_ERROR (Status) ? Status : FdtMapMmio (*Base, Minimum);
}

STATIC EFI_STATUS
ParseControls (CONST VOID *Fdt, INT32 Node, INT32 Child, EIC_PCIE_PORT *Port)
{
  STATIC CONST CHAR8 *ClockNames[] = {"mstr", "dbi", "phy_reg", "aux"};
  // Stable EIC7700 clock/reset binding IDs, not register offsets.
  STATIC CONST UINT32 ClockIds[] = {EIC7700_CLK_GATE_PCIET_ACLK, EIC7700_CLK_GATE_PCIET_CFG_CLK,
                                     EIC7700_CLK_GATE_PCIET_CR_CLK, EIC7700_CLK_GATE_PCIET_AUX_CLK};
  STATIC CONST CHAR8 *ResetNames[] = {"dbi", "pwr"};
  STATIC CONST UINT32 ResetIds[] = {EIC7700_RESET_PCIE_CFG, EIC7700_RESET_PCIE_POWEUP};
  CONST UINT32 *Data;
  INT32 Length, Index, PinNode;
  UINT32 Phandle;
  UINTN Base;
  EFI_STATUS Status;

  Data = FdtGetProp (Fdt, Node, "clocks", &Length);
  if (!Data || Length != ARRAY_SIZE (ClockIds) * 8) { return EFI_UNSUPPORTED; }
  Phandle = Fdt32ToCpu (Data[0]);
  for (UINTN I = 0; I < ARRAY_SIZE (ClockIds); I++) {
    Index = FdtStringListSearch (Fdt, Node, "clock-names", ClockNames[I]);
    if (Index < 0 || Index >= ARRAY_SIZE (ClockIds) ||
        Fdt32ToCpu (Data[Index * 2]) != Phandle ||
        Fdt32ToCpu (Data[Index * 2 + 1]) != ClockIds[I]) {
      return EFI_UNSUPPORTED;
    }
  }
  Status = Provider (Fdt, Phandle, "eswin,eic7700-clock", "#clock-cells",
                     SYSCRG_PCIE_CFG + 4, &Port->Clock);
  if (EFI_ERROR (Status)) { return Status; }

  Data = FdtGetProp (Fdt, Node, "resets", &Length);
  if (!Data || Length != ARRAY_SIZE (ResetIds) * 8) { return EFI_UNSUPPORTED; }
  Phandle = Fdt32ToCpu (Data[0]);
  for (UINTN I = 0; I < ARRAY_SIZE (ResetIds); I++) {
    Index = FdtStringListSearch (Fdt, Node, "reset-names", ResetNames[I]);
    if (Index < 0 || Index >= ARRAY_SIZE (ResetIds) ||
        Fdt32ToCpu (Data[Index * 2]) != Phandle ||
        Fdt32ToCpu (Data[Index * 2 + 1]) != ResetIds[I]) {
      return EFI_UNSUPPORTED;
    }
  }
  Status = Provider (Fdt, Phandle, "eswin,eic7700-reset", "#reset-cells",
                     SYSCRG_PCIE_RESET + 4, &Port->Reset);
  if (EFI_ERROR (Status)) { return Status; }
  Data = FdtGetProp (Fdt, Child, "resets", &Length);
  if (!Data || Length != 8 || Fdt32ToCpu (Data[0]) != Phandle ||
      Fdt32ToCpu (Data[1]) != EIC7700_RESET_PCIE_PERST ||
      FdtStringListSearch (Fdt, Child, "reset-names", "perst") != 0) {
    return EFI_UNSUPPORTED;
  }

  if (EIC7702_MMIO_DIE (Port->Clock) != Port->Segment ||
      EIC7702_MMIO_DIE (Port->Reset) != Port->Segment) {
    return EFI_COMPROMISED_DATA;
  }
  PinNode = -1;
  while ((PinNode = FdtNodeOffsetByCompatible (Fdt, PinNode, "eswin,eic7700-pinctrl")) >= 0) {
    if (!FdtNodeEnabled (Fdt, PinNode)) { continue; }
    Status = FdtGetMmio (Fdt, PinNode, 0, PERST_PAD + 4, &Base, NULL);
    if (EFI_ERROR (Status)) { return Status; }
    if (EIC7702_MMIO_DIE (Base) == Port->Segment) {
      Port->Pins = Base;
      return FdtMapMmio (Base, PERST_PAD + 4);
    }
  }
  return EFI_NOT_FOUND;
}

STATIC VOID
Empty (PCI_ROOT_BRIDGE_APERTURE *Aperture)
{
  Aperture->Base = MAX_UINT64;
  Aperture->Limit = 0;
  Aperture->Translation = 0;
}

STATIC EFI_STATUS
Parse (CONST VOID *Fdt, INT32 Node, EIC_PCIE_PORT *Port, PCI_ROOT_BRIDGE *Bridge)
{
  INT32 RootPort = -1, Child, Parent, Length;
  CONST UINT32 *Data;
  EFI_STATUS Status;

  Parent = FdtParentOffset (Fdt, Node);
  if (Parent < 0 || FdtAddressCells (Fdt, Parent) != 2 ||
      FdtAddressCells (Fdt, Node) != 3 || FdtSizeCells (Fdt, Node) != 2) {
    return EFI_UNSUPPORTED;
  }
  for (Child = FdtFirstSubnode (Fdt, Node); Child >= 0;
       Child = FdtNextSubnode (Fdt, Child)) {
    if (!FdtNodeEnabled (Fdt, Child)) { continue; }
    Data = FdtGetProp (Fdt, Child, "reg", &Length);
    if (!Data || Length != 20 || Data[0] || Data[1] || Data[2] ||
        Data[3] || Data[4] || RootPort >= 0) {
      return EFI_UNSUPPORTED;
    }
    RootPort = Child;
  }
  if (RootPort < 0 || EFI_ERROR (Cell (Fdt, RootPort, "num-lanes", &Port->Lanes)) ||
      (Port->Lanes != 1 && Port->Lanes != 2 && Port->Lanes != 4)) {
    return EFI_UNSUPPORTED;
  }
  Port->MaxSpeed = 3;
  Data = FdtGetProp (Fdt, Node, "max-link-speed", &Length);
  if (Data) {
    if (Length != 4 || !Fdt32ToCpu (*Data) || Fdt32ToCpu (*Data) > 3) {
      return EFI_UNSUPPORTED;
    }
    Port->MaxSpeed = Fdt32ToCpu (*Data);
  }
  Data = FdtGetProp (Fdt, Node, "bus-range", &Length);
  if (!Data || Length != 8 || Data[0] || Fdt32ToCpu (Data[1]) != 255) {
    return EFI_UNSUPPORTED;
  }
  Status = NamedMmio (Fdt, Node, "dbi", DBI_MAP_SIZE, &Port->Dbi);
  if (EFI_ERROR (Status)) { return Status; }
  Status = NamedMmio (Fdt, Node, "config", EFI_PAGE_SIZE, &Port->Config);
  if (EFI_ERROR (Status)) { return Status; }
  Status = NamedMmio (Fdt, Node, "elbi", ELBI_STATUS + 4, &Port->Elbi);
  if (EFI_ERROR (Status)) { return Status; }
  Port->Segment = EIC7702_MMIO_DIE (Port->Dbi);
  if (EIC7702_MMIO_DIE (Port->Config) != Port->Segment ||
      EIC7702_MMIO_DIE (Port->Elbi) != Port->Segment) { return EFI_COMPROMISED_DATA; }
  Status = ParseControls (Fdt, Node, RootPort, Port);
  if (EFI_ERROR (Status)) { return Status; }

  Bridge->Segment = Port->Segment;
  Bridge->Supports = EFI_PCI_IO_ATTRIBUTE_IO | EFI_PCI_IO_ATTRIBUTE_MEMORY |
                     EFI_PCI_IO_ATTRIBUTE_BUS_MASTER | EFI_PCI_ATTRIBUTE_DUAL_ADDRESS_CYCLE;
  Bridge->Attributes = EFI_PCI_IO_ATTRIBUTE_IO | EFI_PCI_IO_ATTRIBUTE_MEMORY |
                       EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
  Bridge->DmaAbove4G = TRUE;
  Bridge->AllocationAttributes = EFI_PCI_HOST_BRIDGE_MEM64_DECODE;
  Bridge->Bus.Base = 0;
  Bridge->Bus.Limit = 255;
  Port->Path.Root.Header.Type = ACPI_DEVICE_PATH;
  Port->Path.Root.Header.SubType = ACPI_DP;
  SetDevicePathNodeLength (&Port->Path.Root.Header, sizeof (Port->Path.Root));
  Port->Path.Root.HID = EISA_PNP_ID (0x0a08);
  Port->Path.Root.UID = Port->Segment;
  SetDevicePathEndNode (&Port->Path.End);
  Bridge->DevicePath = &Port->Path.Root.Header;
  Empty (&Bridge->Io);
  Empty (&Bridge->Mem);
  Empty (&Bridge->PMem);
  Empty (&Bridge->MemAbove4G);
  Empty (&Bridge->PMemAbove4G);

  Data = FdtGetProp (Fdt, Node, "ranges", &Length);
  if (!Data || Length <= 0 || Length % 28 || Length / 28 > MAX_WINDOWS) {
    return EFI_UNSUPPORTED;
  }
  for (UINTN I = 0; I < (UINTN)Length / 28; I++, Data += 7) {
    PCI_WINDOW *Range = &Port->Windows[Port->WindowCount++];
    PCI_ROOT_BRIDGE_APERTURE *Aperture;
    UINT32 Flags = Fdt32ToCpu (Data[0]);
    UINT32 Space = (Flags >> 24) & 3;
    BOOLEAN Above4G;

    Range->Pci = Cells64 (Data + 1);
    Range->Cpu = Cells64 (Data + 3);
    Range->Size = Cells64 (Data + 5);
    if (!Space || !Range->Size || (Range->Pci > MAX_UINT64 - Range->Size) ||
        (Range->Cpu > MAX_UINTN - Range->Size) ||
        ((Range->Pci | Range->Cpu | Range->Size) & EFI_PAGE_MASK) ||
        (Space != 3 && Range->Pci + Range->Size - 1 > MAX_UINT32) ||
        (Space == 1 && (Flags & BIT30))) {
      return EFI_UNSUPPORTED;
    }
    Range->Type = Space == 1 ? ATU_IO : ATU_MEMORY;
    Above4G = Range->Pci > MAX_UINT32;
    if (Space == 3 && !Above4G && Range->Pci + Range->Size - 1 > MAX_UINT32) {
      return EFI_UNSUPPORTED;
    }
    Aperture = Space == 1 ? &Bridge->Io :
               (Flags & BIT30) ? (Above4G ? &Bridge->PMemAbove4G : &Bridge->PMem) :
                                (Above4G ? &Bridge->MemAbove4G : &Bridge->Mem);
    if (Aperture->Base != MAX_UINT64) { return EFI_UNSUPPORTED; }
    Aperture->Base = Range->Pci;
    Aperture->Limit = Range->Pci + Range->Size - 1;
    Aperture->Translation = Range->Pci - Range->Cpu;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Poll (UINTN Address, UINT32 Mask, UINT32 Expected, UINTN TimeoutUs)
{
  UINT32 Value = 0;

  for (UINTN Elapsed = 0; Elapsed < TimeoutUs; Elapsed += 10) {
    Value = MmioRead32 (Address);
    if ((Value & Mask) == Expected) { return EFI_SUCCESS; }
    MicroSecondDelay (10);
  }
  DEBUG ((DEBUG_ERROR, "EIC7700 PCIe poll timeout at %Lx: value %08x mask %08x expected %08x\n",
          (UINT64)Address, Value, Mask, Expected));
  return EFI_TIMEOUT;
}

STATIC BOOLEAN
LinkUp (CONST EIC_PCIE_PORT *Port)
{
  return (MmioRead32 (Port->Elbi + ELBI_STATUS) & ELBI_LINK_UP) == ELBI_LINK_UP;
}

STATIC EFI_STATUS
Window (EIC_PCIE_PORT *Port, UINTN Index, UINT32 Type, UINT64 Cpu, UINT64 Pci, UINT64 Size)
{
  UINTN Base = Port->Dbi + ATU_OFFSET + Index * DWC_ATU_REGION_STRIDE;
  UINT64 Limit;

  if (Index > MAX_WINDOWS || !Size || Cpu > MAX_UINT64 - (Size - 1) ||
      Pci > MAX_UINT64 - (Size - 1) || ((Cpu | Pci | Size) & EFI_PAGE_MASK)) {
    return EFI_INVALID_PARAMETER;
  }
  Limit = Cpu + Size - 1;
  if ((Cpu >> 32) != (Limit >> 32)) { Type |= DWC_ATU_INCREASE_REGION; }
  MmioWrite32 (Base + DWC_ATU_CONTROL2, 0);
  MmioWrite32 (Base + DWC_ATU_LOWER_BASE, Cpu);
  MmioWrite32 (Base + DWC_ATU_UPPER_BASE, Cpu >> 32);
  MmioWrite32 (Base + DWC_ATU_LIMIT, Limit);
  MmioWrite32 (Base + DWC_ATU_LOWER_TARGET, Pci);
  MmioWrite32 (Base + DWC_ATU_UPPER_TARGET, Pci >> 32);
  MmioWrite32 (Base + DWC_ATU_UPPER_LIMIT, Limit >> 32);
  MmioWrite32 (Base + DWC_ATU_CONTROL1, Type);
  MmioWrite32 (Base + DWC_ATU_CONTROL2, DWC_ATU_ENABLE);
  return Poll (Base + DWC_ATU_CONTROL2, DWC_ATU_ENABLE, DWC_ATU_ENABLE, 1000);
}

STATIC EFI_STATUS
PortConfig (EIC_PCIE_PORT *Port, UINT8 Bus, UINT8 Device, UINT8 Function,
        UINT16 Register, UINTN Width, BOOLEAN Write, UINT32 *Value)
{
  EFI_STATUS Status = EFI_SUCCESS;
  EFI_TPL Tpl;
  UINTN Address;

  if (!Value || Device > 31 || Function > 7 ||
      (Width != 1 && Width != 2 && Width != 4) || Register >= 4096 ||
      (Register & (Width - 1)) || Width > 4096U - Register) {
    return EFI_INVALID_PARAMETER;
  }
  if ((!Bus && (Device || Function)) || (Bus == 1 && Device)) { return EFI_NOT_FOUND; }
  if (Bus && !LinkUp (Port)) { return EFI_NOT_READY; }
  Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  Address = (Bus ? Port->Config : Port->Dbi) + Register;
  if (Bus) {
    Status = Window (Port, 0, Bus == 1 ? ATU_CONFIG0 : ATU_CONFIG1, Port->Config,
                     ((UINT32)Bus << 24) | ((UINT32)Device << 19) | ((UINT32)Function << 16),
                     EFI_PAGE_SIZE);
  }
  if (!EFI_ERROR (Status)) {
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
  gBS->RestoreTPL (Tpl);
  return Status;
}

STATIC EFI_STATUS EFIAPI
Config (UINT16 Segment, UINT8 Bus, UINT8 Device, UINT8 Function,
        UINT16 Register, UINTN Width, BOOLEAN Write, UINT32 *Value)
{
  for (UINTN I = 0; I < mHost.Count; I++) {
    EIC_PCIE_PORT *Port = &mPorts[I];
    if (Segment == Port->Segment) {
      if (Bus && !Port->Trained) { return EFI_NOT_FOUND; }
      return PortConfig (Port, Bus, Device, Function, Register, Width, Write, Value);
    }
  }
  return EFI_NOT_FOUND;
}

STATIC BOOLEAN EFIAPI
LinkReady (VOID *Context)
{
  EIC_PCIE_PORT *Port = Context;
  return (MmioRead32 (Port->Elbi + ELBI_STATUS) & (ELBI_PHY_AUX_CLOCK | 0xff)) == ELBI_L0_STATUS;
}

STATIC VOID EFIAPI
EnableLtssm (VOID *Context)
{
  EIC_PCIE_PORT *Port = Context;
  MmioOr32 (Port->Elbi + ELBI_CONTROL, ELBI_LTSSM_ENABLE);
}

STATIC EFI_STATUS EFIAPI
ReadPartner (VOID *Context, UINT16 Register, UINT32 *Value)
{
  EIC_PCIE_PORT *Port = Context;
  return PortConfig (Port, 1, 0, 0, Register, sizeof (*Value), FALSE, Value);
}

STATIC VOID
Stop (EIC_PCIE_PORT *Port)
{
  // Power-up reset must precede LTSSM disable outside Detect.
  MmioAnd32 (Port->Reset + SYSCRG_PCIE_RESET, ~(UINT32)(RESET_PERST | RESET_POWER));
  MmioAndThenOr32 (Port->Elbi + ELBI_CONTROL, ~(UINT32)ELBI_LTSSM_ENABLE, ELBI_HOLD_PHY);
  MmioAnd32 (Port->Reset + SYSCRG_PCIE_RESET, ~(UINT32)RESET_DBI);
  MmioAnd32 (Port->Clock + SYSCRG_PCIE_CFG, ~(UINT32)(CLOCK_AUX | CLOCK_CR | CLOCK_GATE));
  MmioAnd32 (Port->Clock + SYSCRG_PCIE_ACLK, ~(UINT32)CLOCK_GATE);
  MmioWrite32 (Port->Pins + PERST_PAD, Port->SavedPad);
}

STATIC EFI_STATUS
Start (EIC_PCIE_PORT *Port, BOOLEAN DualDie)
{
  UINT32 Version;
  UINT16 LinkStatus;
  EFI_STATUS Status;
  DWC_PCIE_TRAINING Training = {
    DualDie ? (Port->Segment ? "EIC7702 die 1" : "EIC7702 die 0") : "EIC7700",
    Port->Dbi, Port->MaxSpeed, Port->Lanes, Port, LinkReady, EnableLtssm, ReadPartner,
    DualDie, DualDie
  };

  Port->SavedPad = MmioRead32 (Port->Pins + PERST_PAD);
  MmioOr32 (Port->Pins + PERST_PAD, PAD_INPUT_ENABLE);
  // TCU AXI must run for incoming DMA, including SMMU bypass mode.
  MmioOr32 (Port->Clock + SYSCRG_TCU_ACLK, CLOCK_GATE);
  MmioOr32 (Port->Clock + SYSCRG_PCIE_CFG, CLOCK_CR);
  MmioOr32 (Port->Clock + SYSCRG_PCIE_ACLK, CLOCK_GATE);
  MmioOr32 (Port->Clock + SYSCRG_PCIE_CFG, CLOCK_GATE | CLOCK_AUX);
  MmioAnd32 (Port->Reset + SYSCRG_PCIE_RESET,
             ~(UINT32)(RESET_DBI | RESET_POWER | RESET_PERST));
  MmioOr32 (Port->Reset + SYSCRG_PCIE_RESET, RESET_DBI);
  MmioOr32 (Port->Reset + SYSCRG_PCIE_RESET, RESET_POWER);
  MmioAndThenOr32 (Port->Elbi + ELBI_CONTROL,
                   ~(ELBI_DEVICE_TYPE | ELBI_LTSSM_ENABLE),
                   ELBI_ROOT_PORT | ELBI_HOLD_PHY);
  // Keep the endpoint reset while controller clocks and reference clock settle.
  MicroSecondDelay (100000);
  // PERST# also resets this SoC's PHY. Release it before waiting for PHY ready.
  MmioOr32 (Port->Reset + SYSCRG_PCIE_RESET, RESET_PERST);
  MmioAnd32 (Port->Elbi + ELBI_CONTROL, ~(UINT32)ELBI_HOLD_PHY);
  Status = Poll (Port->Elbi + ELBI_STATUS, ELBI_PHY_AUX_CLOCK, 0, 20000);
  if (EFI_ERROR (Status)) { return Status; }
  Version = MmioRead32 (Port->Dbi + DBI_VERSION);
  if (Version < 0x3438302a || Version == MAX_UINT32) { return EFI_UNSUPPORTED; }

  // Root mode does not use the endpoint-facing local register BARs.
  // Disable their masks through EIC7700's shadow/CS2 window before sizing.
  MmioWrite32 (Port->Dbi + DBI_CS2_OFFSET + PCI_BASE_ADDRESSREG_OFFSET, 0);
  MmioWrite32 (Port->Dbi + DBI_CS2_OFFSET + PCI_BASE_ADDRESSREG_OFFSET + 4, 0);
  MmioOr32 (Port->Dbi + DWC_DBI_MISC_CONTROL, DWC_DBI_RO_WRITE_ENABLE);
  MmioWrite32 (Port->Dbi, DBI_VENDOR_DEVICE);
  MmioWrite16 (Port->Dbi + PCI_CLASSCODE_OFFSET + 1, 0x0604);
  MmioWrite32 (Port->Dbi + PCI_BASE_ADDRESSREG_OFFSET, 0);
  MmioWrite32 (Port->Dbi + PCI_BASE_ADDRESSREG_OFFSET + 4, 0);
  MmioWrite32 (Port->Dbi + PCI_BRIDGE_PRIMARY_BUS_REGISTER_OFFSET, 0x00ff0100);
  MmioOr16 (Port->Dbi + PCI_COMMAND_OFFSET,
            EFI_PCI_COMMAND_MEMORY_SPACE | EFI_PCI_COMMAND_BUS_MASTER);
  MmioAndThenOr32 (Port->Dbi + DBI_MSI_CAP, ~(7U << 17), 5U << 17);
  MmioAnd32 (Port->Dbi + DBI_EXP_CAP, ~0xff00U);
  MmioAndThenOr32 (Port->Dbi + DBI_PORT_LINK, ~(DBI_LINK_MODE_MASK | DBI_FAST_LINK),
                   ((2 * Port->Lanes - 1) << 16) | DBI_DLL_ENABLE);
  MmioAndThenOr32 (Port->Dbi + DBI_GEN2_CONTROL, ~DBI_PREDETERMINED_LANES, 1U << 8);
  MmioAndThenOr32 (Port->Dbi + DBI_LINK_CAP, ~0x3ffU, (Port->Lanes << 4) | Port->MaxSpeed);
  MmioAnd32 (Port->Dbi + DBI_MULTI_LANE, ~0x7fU);
  MmioAnd32 (Port->Dbi + DWC_DBI_MISC_CONTROL, ~(UINT32)DWC_DBI_RO_WRITE_ENABLE);
  for (UINTN I = 0; I < Port->WindowCount; I++) {
    PCI_WINDOW *Range = &Port->Windows[I];
    Status = Window (Port, I + 1, Range->Type, Range->Cpu, Range->Pci, Range->Size);
    if (EFI_ERROR (Status)) { return Status; }
  }
  Status = DwcPcieTrainLink (&Training, &LinkStatus);
  if (Status == EFI_NO_MEDIA && DualDie) {
    DEBUG ((DEBUG_INFO, "EIC7702 PCIe segment %u: no active link\n", Port->Segment));
    return EFI_SUCCESS;
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "EIC7700 PCIe ELBI status: %08x\n", MmioRead32 (Port->Elbi + ELBI_STATUS)));
    return Status;
  }
  DEBUG ((DEBUG_INFO, "EIC7700 PCIe: Gen%u x%u, DesignWare %08x\n",
          LinkStatus & 0xf, (LinkStatus >> 4) & 0x3f, Version));
  Port->Trained = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
Eic7700PciEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  RUYI_BOARD_INFO Info;
  CONST VOID *Fdt;
  EFI_HANDLE Handle = NULL;
  EFI_STATUS Status, Last = EFI_NOT_FOUND;
  INT32 Node = -1;
  BOOLEAN DualDie;

  Status = FdtBoardInfoGet (&Info, &Fdt);
  if (EFI_ERROR (Status)) { return Status; }
  DualDie = FdtStringListSearch (Fdt, 0, "compatible", "eswin,eic7702") >= 0;
  while ((Node = FdtNodeOffsetByCompatible (Fdt, Node, "eswin,eic7700-pcie")) >= 0) {
    if (!FdtNodeEnabled (Fdt, Node)) { continue; }
    UINTN I = mHost.Count;
    if (I == (DualDie ? MAX_ROOTS : 1)) { break; }
    EIC_PCIE_PORT *Port = &mPorts[I];
    PCI_ROOT_BRIDGE *Root = &mRoots[I];
    ZeroMem (Port, sizeof (*Port));
    ZeroMem (Root, sizeof (*Root));
    Last = Parse (Fdt, Node, Port, Root);
    if (EFI_ERROR (Last)) { continue; }
    if (I && Port->Segment == mPorts[0].Segment) { Last = EFI_COMPROMISED_DATA; continue; }
    Last = Start (Port, DualDie);
    if (EFI_ERROR (Last)) {
      DEBUG ((DEBUG_ERROR, "EIC7700 PCIe segment %u initialization failed: %r\n", Port->Segment, Last));
      Stop (Port);
      continue;
    }
    mIoBases[I] = Root->Io.Base - Root->Io.Translation;
    mHost.Count++;
  }
  if (mHost.Count == 0) { return Last; }
  mHost.Controller.Revision = DualDie ? 2 : 1;
  mHost.Controller.Config = Config;
  mHost.Controller.RootBridge = mRoots[0];
  mHost.Bridges = mRoots;
  mHost.IoMmioBases = mIoBases;
  Status = gBS->InstallProtocolInterface (&Handle, &gRuyiPciControllerProtocolGuid,
                                          EFI_NATIVE_INTERFACE, &mHost);
  if (EFI_ERROR (Status)) {
    while (mHost.Count) { Stop (&mPorts[--mHost.Count]); }
  }
  return Status;
}
