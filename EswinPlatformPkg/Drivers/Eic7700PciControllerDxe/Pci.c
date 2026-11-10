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
#include <Uefi.h>
#include <IndustryStandard/Pci.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
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
#define DBI_MISC_CONTROL       0x8bc
#define DBI_RO_WRITE           BIT0
#define DBI_MULTI_LANE         0x8c0
#define DBI_VERSION            0x8f8
#define ATU_OFFSET             0x300000
#define ATU_STRIDE             0x200
#define ATU_ENABLE             BIT31
#define ATU_INCREASE_REGION    BIT13
#define ATU_MEMORY             0U
#define ATU_IO                 2U
#define ATU_CONFIG0            4U
#define ATU_CONFIG1            5U
#define MAX_WINDOWS            4
#define DBI_MAP_SIZE           (ATU_OFFSET + (MAX_WINDOWS + 1) * ATU_STRIDE)

STATIC UINTN   mDbi, mConfig, mElbi, mClock, mReset, mPins;
STATIC UINT32  mLanes, mMaxSpeed = 3;
STATIC UINT32  mSavedPad;
STATIC RUYI_PCI_CONTROLLER_PROTOCOL mController;
STATIC struct {
  ACPI_HID_DEVICE_PATH      Root;
  EFI_DEVICE_PATH_PROTOCOL End;
} mPath = {
  {{ACPI_DEVICE_PATH, ACPI_DP, {sizeof (ACPI_HID_DEVICE_PATH), 0}},
   EISA_PNP_ID (0x0a08), 0},
  {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
   {sizeof (EFI_DEVICE_PATH_PROTOCOL), 0}}
};

typedef struct {
  UINT64 Cpu, Pci, Size;
  UINT32 Type;
} PCI_WINDOW;
STATIC PCI_WINDOW mWindows[MAX_WINDOWS];
STATIC UINTN mWindowCount;

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
ParseControls (CONST VOID *Fdt, INT32 Node, INT32 Port)
{
  STATIC CONST CHAR8 *ClockNames[] = {"mstr", "dbi", "phy_reg", "aux"};
  // Stable EIC7700 clock/reset binding IDs, not register offsets.
  STATIC CONST UINT32 ClockIds[] = {144, 145, 146, 147};
  STATIC CONST CHAR8 *ResetNames[] = {"dbi", "pwr"};
  STATIC CONST UINT32 ResetIds[] = {97, 98};
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
                     SYSCRG_PCIE_CFG + 4, &mClock);
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
                     SYSCRG_PCIE_RESET + 4, &mReset);
  if (EFI_ERROR (Status)) { return Status; }
  Data = FdtGetProp (Fdt, Port, "resets", &Length);
  if (!Data || Length != 8 || Fdt32ToCpu (Data[0]) != Phandle ||
      Fdt32ToCpu (Data[1]) != 99 ||
      FdtStringListSearch (Fdt, Port, "reset-names", "perst") != 0) {
    return EFI_UNSUPPORTED;
  }

  PinNode = FdtNodeOffsetByCompatible (Fdt, -1, "eswin,eic7700-pinctrl");
  if (!FdtNodeEnabled (Fdt, PinNode)) { return EFI_NOT_FOUND; }
  Status = FdtGetMmio (Fdt, PinNode, 0, PERST_PAD + 4, &Base, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (Base, PERST_PAD + 4);
  if (!EFI_ERROR (Status)) { mPins = Base; }
  return Status;
}

STATIC VOID
Empty (PCI_ROOT_BRIDGE_APERTURE *Aperture)
{
  Aperture->Base = MAX_UINT64;
  Aperture->Limit = 0;
  Aperture->Translation = 0;
}

STATIC EFI_STATUS
Parse (CONST VOID *Fdt)
{
  INT32 Node, Port = -1, Child, Parent, Length;
  CONST UINT32 *Data;
  PCI_ROOT_BRIDGE *Bridge = &mController.RootBridge;
  EFI_STATUS Status;

  Node = FdtNodeOffsetByCompatible (Fdt, -1, "eswin,eic7700-pcie");
  if (!FdtNodeEnabled (Fdt, Node)) { return EFI_NOT_FOUND; }
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
        Data[3] || Data[4] || Port >= 0) {
      return EFI_UNSUPPORTED;
    }
    Port = Child;
  }
  if (Port < 0 || EFI_ERROR (Cell (Fdt, Port, "num-lanes", &mLanes)) ||
      (mLanes != 1 && mLanes != 2 && mLanes != 4)) {
    return EFI_UNSUPPORTED;
  }
  Data = FdtGetProp (Fdt, Node, "max-link-speed", &Length);
  if (Data) {
    if (Length != 4 || !Fdt32ToCpu (*Data) || Fdt32ToCpu (*Data) > 3) {
      return EFI_UNSUPPORTED;
    }
    mMaxSpeed = Fdt32ToCpu (*Data);
  }
  Data = FdtGetProp (Fdt, Node, "bus-range", &Length);
  if (!Data || Length != 8 || Data[0] || Fdt32ToCpu (Data[1]) != 255) {
    return EFI_UNSUPPORTED;
  }
  Status = NamedMmio (Fdt, Node, "dbi", DBI_MAP_SIZE, &mDbi);
  if (EFI_ERROR (Status)) { return Status; }
  Status = NamedMmio (Fdt, Node, "config", EFI_PAGE_SIZE, &mConfig);
  if (EFI_ERROR (Status)) { return Status; }
  Status = NamedMmio (Fdt, Node, "elbi", ELBI_STATUS + 4, &mElbi);
  if (EFI_ERROR (Status)) { return Status; }
  Status = ParseControls (Fdt, Node, Port);
  if (EFI_ERROR (Status)) { return Status; }

  Bridge->Segment = 0;
  Bridge->Supports = EFI_PCI_IO_ATTRIBUTE_IO | EFI_PCI_IO_ATTRIBUTE_MEMORY |
                     EFI_PCI_IO_ATTRIBUTE_BUS_MASTER | EFI_PCI_ATTRIBUTE_DUAL_ADDRESS_CYCLE;
  Bridge->Attributes = EFI_PCI_IO_ATTRIBUTE_IO | EFI_PCI_IO_ATTRIBUTE_MEMORY |
                       EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
  Bridge->DmaAbove4G = TRUE;
  Bridge->AllocationAttributes = EFI_PCI_HOST_BRIDGE_MEM64_DECODE;
  Bridge->Bus.Base = 0;
  Bridge->Bus.Limit = 255;
  Bridge->DevicePath = (VOID *)&mPath;
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
    PCI_WINDOW *Range = &mWindows[mWindowCount++];
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
LinkUp (VOID)
{
  return (MmioRead32 (mElbi + ELBI_STATUS) & ELBI_LINK_UP) == ELBI_LINK_UP;
}

STATIC EFI_STATUS
Window (UINTN Index, UINT32 Type, UINT64 Cpu, UINT64 Pci, UINT64 Size)
{
  UINTN Base = mDbi + ATU_OFFSET + Index * ATU_STRIDE;
  UINT64 Limit;

  if (Index > MAX_WINDOWS || !Size || Cpu > MAX_UINT64 - (Size - 1) ||
      Pci > MAX_UINT64 - (Size - 1) || ((Cpu | Pci | Size) & EFI_PAGE_MASK)) {
    return EFI_INVALID_PARAMETER;
  }
  Limit = Cpu + Size - 1;
  if ((Cpu >> 32) != (Limit >> 32)) { Type |= ATU_INCREASE_REGION; }
  MmioWrite32 (Base + 4, 0);
  MmioWrite32 (Base + 8, Cpu);
  MmioWrite32 (Base + 12, Cpu >> 32);
  MmioWrite32 (Base + 16, Limit);
  MmioWrite32 (Base + 20, Pci);
  MmioWrite32 (Base + 24, Pci >> 32);
  MmioWrite32 (Base + 32, Limit >> 32);
  MmioWrite32 (Base, Type);
  MmioWrite32 (Base + 4, ATU_ENABLE);
  return Poll (Base + 4, ATU_ENABLE, ATU_ENABLE, 1000);
}

STATIC EFI_STATUS EFIAPI
Config (UINT16 Segment, UINT8 Bus, UINT8 Device, UINT8 Function,
        UINT16 Register, UINTN Width, BOOLEAN Write, UINT32 *Value)
{
  EFI_STATUS Status = EFI_SUCCESS;
  EFI_TPL Tpl;
  UINTN Address;

  if (!Value || Segment || Device > 31 || Function > 7 ||
      (Width != 1 && Width != 2 && Width != 4) || Register >= 4096 ||
      (Register & (Width - 1)) || Width > 4096U - Register) {
    return EFI_INVALID_PARAMETER;
  }
  if ((!Bus && (Device || Function)) || (Bus == 1 && Device)) { return EFI_NOT_FOUND; }
  if (Bus && !LinkUp ()) { return EFI_NOT_READY; }
  Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  Address = (Bus ? mConfig : mDbi) + Register;
  if (Bus) {
    Status = Window (0, Bus == 1 ? ATU_CONFIG0 : ATU_CONFIG1, mConfig,
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

STATIC BOOLEAN EFIAPI
LinkReady (VOID *Context)
{
  return (MmioRead32 (mElbi + ELBI_STATUS) & (ELBI_PHY_AUX_CLOCK | 0xff)) == ELBI_L0_STATUS;
}

STATIC VOID EFIAPI
EnableLtssm (VOID *Context)
{
  MmioOr32 (mElbi + ELBI_CONTROL, ELBI_LTSSM_ENABLE);
}

STATIC EFI_STATUS EFIAPI
ReadPartner (VOID *Context, UINT16 Register, UINT32 *Value)
{
  return Config (0, 1, 0, 0, Register, sizeof (*Value), FALSE, Value);
}

STATIC VOID
Stop (VOID)
{
  // Power-up reset must precede LTSSM disable outside Detect.
  MmioAnd32 (mReset + SYSCRG_PCIE_RESET, ~(UINT32)(RESET_PERST | RESET_POWER));
  MmioAndThenOr32 (mElbi + ELBI_CONTROL, ~(UINT32)ELBI_LTSSM_ENABLE, ELBI_HOLD_PHY);
  MmioAnd32 (mReset + SYSCRG_PCIE_RESET, ~(UINT32)RESET_DBI);
  MmioAnd32 (mClock + SYSCRG_PCIE_CFG, ~(UINT32)(CLOCK_AUX | CLOCK_CR | CLOCK_GATE));
  MmioAnd32 (mClock + SYSCRG_PCIE_ACLK, ~(UINT32)CLOCK_GATE);
  MmioWrite32 (mPins + PERST_PAD, mSavedPad);
}

STATIC EFI_STATUS
Start (VOID)
{
  UINT32 Version;
  UINT16 LinkStatus;
  EFI_STATUS Status;
  DWC_PCIE_TRAINING Training = {
    "EIC7700", mDbi, mMaxSpeed, mLanes, NULL, LinkReady, EnableLtssm, ReadPartner
  };

  mSavedPad = MmioRead32 (mPins + PERST_PAD);
  MmioOr32 (mPins + PERST_PAD, PAD_INPUT_ENABLE);
  // TCU AXI must run for incoming DMA, including SMMU bypass mode.
  MmioOr32 (mClock + SYSCRG_TCU_ACLK, CLOCK_GATE);
  MmioOr32 (mClock + SYSCRG_PCIE_CFG, CLOCK_CR);
  MmioOr32 (mClock + SYSCRG_PCIE_ACLK, CLOCK_GATE);
  MmioOr32 (mClock + SYSCRG_PCIE_CFG, CLOCK_GATE | CLOCK_AUX);
  MmioAnd32 (mReset + SYSCRG_PCIE_RESET,
             ~(UINT32)(RESET_DBI | RESET_POWER | RESET_PERST));
  MmioOr32 (mReset + SYSCRG_PCIE_RESET, RESET_DBI);
  MmioOr32 (mReset + SYSCRG_PCIE_RESET, RESET_POWER);
  MmioAndThenOr32 (mElbi + ELBI_CONTROL,
                   ~(ELBI_DEVICE_TYPE | ELBI_LTSSM_ENABLE),
                   ELBI_ROOT_PORT | ELBI_HOLD_PHY);
  // Keep the endpoint reset while controller clocks and reference clock settle.
  MicroSecondDelay (100000);
  // PERST# also resets this SoC's PHY. Release it before waiting for PHY ready.
  MmioOr32 (mReset + SYSCRG_PCIE_RESET, RESET_PERST);
  MmioAnd32 (mElbi + ELBI_CONTROL, ~(UINT32)ELBI_HOLD_PHY);
  Status = Poll (mElbi + ELBI_STATUS, ELBI_PHY_AUX_CLOCK, 0, 20000);
  if (EFI_ERROR (Status)) { return Status; }
  Version = MmioRead32 (mDbi + DBI_VERSION);
  if (Version < 0x3438302a || Version == MAX_UINT32) { return EFI_UNSUPPORTED; }

  // Root mode does not use the endpoint-facing local register BARs.
  // Disable their masks through EIC7700's shadow/CS2 window before sizing.
  MmioWrite32 (mDbi + DBI_CS2_OFFSET + PCI_BASE_ADDRESSREG_OFFSET, 0);
  MmioWrite32 (mDbi + DBI_CS2_OFFSET + PCI_BASE_ADDRESSREG_OFFSET + 4, 0);
  MmioOr32 (mDbi + DBI_MISC_CONTROL, DBI_RO_WRITE);
  MmioWrite32 (mDbi, DBI_VENDOR_DEVICE);
  MmioWrite16 (mDbi + PCI_CLASSCODE_OFFSET + 1, 0x0604);
  MmioWrite32 (mDbi + PCI_BASE_ADDRESSREG_OFFSET, 0);
  MmioWrite32 (mDbi + PCI_BASE_ADDRESSREG_OFFSET + 4, 0);
  MmioWrite32 (mDbi + PCI_BRIDGE_PRIMARY_BUS_REGISTER_OFFSET, 0x00ff0100);
  MmioOr16 (mDbi + PCI_COMMAND_OFFSET,
            EFI_PCI_COMMAND_MEMORY_SPACE | EFI_PCI_COMMAND_BUS_MASTER);
  MmioAndThenOr32 (mDbi + DBI_MSI_CAP, ~(7U << 17), 5U << 17);
  MmioAnd32 (mDbi + DBI_EXP_CAP, ~0xff00U);
  MmioAndThenOr32 (mDbi + DBI_PORT_LINK, ~(DBI_LINK_MODE_MASK | DBI_FAST_LINK),
                   ((2 * mLanes - 1) << 16) | DBI_DLL_ENABLE);
  // As in Linux DWC, require one lane to exit electrical idle, then negotiate.
  MmioAndThenOr32 (mDbi + DBI_GEN2_CONTROL, ~DBI_PREDETERMINED_LANES, 1U << 8);
  MmioAndThenOr32 (mDbi + DBI_LINK_CAP, ~0x3ffU, (mLanes << 4) | mMaxSpeed);
  MmioAnd32 (mDbi + DBI_MULTI_LANE, ~0x7fU);
  MmioAnd32 (mDbi + DBI_MISC_CONTROL, ~(UINT32)DBI_RO_WRITE);
  Status = DwcPcieTrainLink (&Training, &LinkStatus);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "EIC7700 PCIe ELBI status: %08x\n", MmioRead32 (mElbi + ELBI_STATUS)));
    return Status;
  }
  DEBUG ((DEBUG_INFO, "EIC7700 PCIe: Gen%u x%u, DesignWare %08x\n",
          LinkStatus & 0xf, (LinkStatus >> 4) & 0x3f, Version));
  for (UINTN I = 0; I < mWindowCount; I++) {
    PCI_WINDOW *Range = &mWindows[I];
    Status = Window (I + 1, Range->Type, Range->Cpu, Range->Pci, Range->Size);
    if (EFI_ERROR (Status)) { return Status; }
  }
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
Eic7700PciEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  RUYI_BOARD_INFO Info;
  CONST VOID *Fdt;
  EFI_HANDLE Handle = NULL;
  EFI_STATUS Status;

  Status = FdtBoardInfoGet (&Info, &Fdt);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Parse (Fdt);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Start ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "EIC7700 PCIe initialization failed: %r\n", Status));
    Stop ();
    return Status;
  }
  mController.Revision = 1;
  mController.Config = Config;
  Status = gBS->InstallProtocolInterface (&Handle, &gRuyiPciControllerProtocolGuid,
                                         EFI_NATIVE_INTERFACE, &mController);
  if (EFI_ERROR (Status)) { Stop (); }
  return Status;
}
