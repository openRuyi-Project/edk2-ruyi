/** SPDX-License-Identifier: BSD-2-Clause-Patent */

/* K3 controller fields follow the pinned K3 PCIe integration references.
 * Copyright (c) 2017, Linaro Ltd. All rights reserved.
 * Copyright (c) 2019 Marvell International Ltd. All rights reserved.
 * Copyright (c) 2025 Spacemit Ltd. All rights reserved.
 */
#include <PiDxe.h>
#include <Guid/RiscVSecHobData.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/K3HardwareLib.h>
#include <Library/DesignWarePciLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/ComboPhy.h>
#include <Protocol/PciController.h>

STATIC SPACEMIT_PCIE_PORT  mPorts[SPACEMIT_PCIE_MAX_PORTS];
STATIC UINTN               mCount;
STATIC UINTN               mParentRegister;
STATIC BOOLEAN             mParentAlreadyEnabled;
#define CLK_RESET_BITS  0x3fU
#define LTSSM           BIT6
#define HOLD_PHY        BIT30
#define AUX_POWER       BIT9
#define IGNORE_PERST    BIT31
#define PERST_OE        BIT24
#define PERST_OUT       BIT25
#define LINK_UP_BITS    (BIT1 | BIT12)
#define LTSSM_MASK      (0x3fU << 6)
#define LTSSM_L0        (0x11U << 6)

STATIC BOOLEAN
WaitL0 (
  SPACEMIT_PCIE_PORT  *Port,
  UINT32              *Link
  )
{
  for (UINTN Elapsed = 0; Elapsed < 1000000; Elapsed += 1000) {
    *Link = MmioRead32 (Port->Link + 4);
    if ((*Link & (LINK_UP_BITS | LTSSM_MASK)) ==
        (LINK_UP_BITS | LTSSM_L0))
    {
      return TRUE;
    }

    MicroSecondDelay (1000);
  }

  return FALSE;
}

STATIC VOID
Park (
  SPACEMIT_PCIE_PORT  *Port
  )
{
  Port->Ready = FALSE;
  MmioAndThenOr32 (Port->Control + 4, ~PERST_OUT, PERST_OE | IGNORE_PERST);
  MmioAndThenOr32 (Port->Control, ~LTSSM, HOLD_PHY);
  for (UINTN I = 0; I < 4; I++) {
    MmioWrite32 (Port->Atu + I * 0x200 + 4, 0);
  }

  // Disable DBI/ATU clocks only after all dependent register writes finish.
  MmioAnd32 (Port->Control, ~CLK_RESET_BITS);
}

STATIC EFI_STATUS EFIAPI
Config (
  UINT16   Segment,
  UINT8    Bus,
  UINT8    Device,
  UINT8    Function,
  UINT16   Register,
  UINTN    Width,
  BOOLEAN  Write,
  UINT32   *Value
  )
{
  for (UINTN I = 0; I < mCount; I++) {
    if (mPorts[I].Segment == Segment) {
      return SpacemitPcieConfigAccess (&mPorts[I], Bus, Device, Function, Register, Width, Write, Value);
    }
  }

  return EFI_NOT_FOUND;
}

STATIC EFI_STATUS
EnableParent (
  CONST VOID  *Fdt
  )
{
  INT32       Pll = FdtNodeOffsetByCompatible (Fdt, -1, "spacemit,k3-pll"), Length;
  UINTN       Base, Bytes, Mpmu;
  EFI_STATUS  Status;

  if (!K3FdtEnabled (Fdt, Pll)) {
    return EFI_NOT_FOUND;
  }

  Status = K3FdtRegister (Fdt, Pll, 0, 0x124, &Base, &Bytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3MapMmio (Base, Bytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CONST UINT32  *Ref = FdtGetProp (Fdt, Pll, "spacemit,mpmu", &Length);

  if (!Ref || (Length != 4)) {
    return EFI_COMPROMISED_DATA;
  }

  INT32  Node = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Ref));

  if (!K3FdtEnabled (Fdt, Node) || (FdtStringListSearch (Fdt, Node, "compatible", "spacemit,k3-syscon-mpmu") < 0)) {
    return EFI_COMPROMISED_DATA;
  }

  Status = K3FdtRegister (Fdt, Node, 0, 0x14, &Mpmu, &Bytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3MapMmio (Mpmu, Bytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // The common master/slave clock is PLL2 / 6. Validate the BootROM VCO,
  // enable only its divider output, and never retune a shared parent here.
  UINT32  Control = MmioRead32 (Base + 0x11c);

  if ((MmioRead32 (Base + 0x118) != 0x0b3e2000) ||
      (MmioRead32 (Base + 0x120) != 0xa0558c8c) || (Control & 0xff00) ||
      !(Control & BIT16) || !(MmioRead32 (Mpmu + 0x10) & BIT25))
  {
    return EFI_NOT_READY;
  }

  mParentRegister       = Base + 0x11c;
  mParentAlreadyEnabled = (Control & BIT5) != 0;
  MmioOr32 (mParentRegister, BIT5);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Binding (
  CONST VOID          *Fdt,
  SPACEMIT_PCIE_PORT  *Port,
  INT32               *Child
  )
{
  INT32         Length, Parent, Clk;
  CONST UINT32  *Cells;
  CONST UINT32  *ControlRef = FdtGetProp (Fdt, Port->Node, "spacemit,apmu", &Length);

  if (!ControlRef || (Length != 8)) {
    return EFI_COMPROMISED_DATA;
  }

  INT32  ControlProvider = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (ControlRef[0]));

  if ((Port->Dbi < 0x80000000) || (Port->Dbi > 0x81000000) ||
      (Port->Dbi - 0x80000000) % 0x400000)
  {
    return EFI_UNSUPPORTED;
  }

  UINT32              Id       = (UINT32)((Port->Dbi - 0x80000000) / 0x400000);
  static CONST CHAR8  *Names[] = { "dbi", "mstr", "slv" };

  Cells = FdtGetProp (Fdt, Port->Node, "clocks", &Length);
  if (!Cells || (Length != 24)) {
    return EFI_COMPROMISED_DATA;
  }

  for (UINT32 I = 0; I < 3; I++) {
    INT32  Index = FdtStringListSearch (Fdt, Port->Node, "clock-names", Names[I]);
    if ((Index < 0) || (Index >= 3)) {
      return EFI_COMPROMISED_DATA;
    }

    Clk = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Cells[Index * 2]));
    CONST UINT32  *ClockCells = FdtGetProp (Fdt, Clk, "#clock-cells", &Length);
    if ((Clk != ControlProvider) ||
        !K3FdtEnabled (Fdt, Clk) || !ClockCells || (Length != 4) || (Fdt32ToCpu (*ClockCells) != 1) ||
        (FdtStringListSearch (Fdt, Clk, "compatible", "spacemit,k3-syscon-apmu") < 0))
    {
      return EFI_UNSUPPORTED;
    }

    UINT32  Expected = I == 0 ? 89 + Id : 56 + Id * 2 + (I == 2 ? 1 : 0);
    if (Fdt32ToCpu (Cells[Index * 2 + 1]) != Expected) {
      return EFI_COMPROMISED_DATA;
    }
  }

  UINTN                Apmu, Size;
  EFI_STATUS           Status    = K3FdtRegister (Fdt, ControlProvider, 0, 0x200, &Apmu, &Size);
  static CONST UINT32  Offsets[] = { 0x1f0, 0x1f8, 0x1c8, 0x1b8, 0x1e8 };

  if (EFI_ERROR (Status) || (Port->Control != Apmu + Offsets[Id])) {
    return EFI_COMPROMISED_DATA;
  }

  Cells = FdtGetProp (Fdt, Port->Node, "resets", &Length);
  if (!Cells || (Length != 24)) {
    return EFI_COMPROMISED_DATA;
  }

  for (UINT32 I = 0; I < 3; I++) {
    INT32  Index = FdtStringListSearch (Fdt, Port->Node, "reset-names", Names[I]);
    if ((Index < 0) || (Index >= 3) ||
        (FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Cells[Index * 2])) != ControlProvider) ||
        (Fdt32ToCpu (Cells[Index * 2 + 1]) != 81 + Id * 3 + ((I == 0) ? 0 : (I == 1) ? 2 : 1)))
    {
      return EFI_COMPROMISED_DATA;
    }
  }

  *Child = -1;
  for (Parent = FdtFirstSubnode (Fdt, Port->Node); Parent >= 0; Parent = FdtNextSubnode (Fdt, Parent)) {
    CONST CHAR8  *Type = FdtGetProp (Fdt, Parent, "device_type", &Length);
    if (K3FdtEnabled (Fdt, Parent) && Type && (Length == 4) && !AsciiStrCmp (Type, "pci")) {
      if (*Child >= 0) {
        return EFI_UNSUPPORTED;
      }

      *Child = Parent;
    }
  }

  if (*Child < 0) {
    return EFI_COMPROMISED_DATA;
  }

  return K3RequireFixedSupply (Fdt, *Child, "vpcie3v3-supply", 3300000);
}

STATIC EFI_STATUS
Start (
  CONST VOID                   *Fdt,
  SPACEMIT_PCIE_PORT           *Port,
  SPACEMIT_COMBO_PHY_PROTOCOL  *Phy
  )
{
  EFI_STATUS  Status;
  INT32       Child;
  UINT32      Value;

  Status = Binding (Fdt, Port, &Child);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3ApplyPins (Fdt, Port->Node);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3MapMmio (Port->Control, 8);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3MapMmio (Port->Dbi, 4096);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3MapMmio (Port->Dbi2, 4096);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3MapMmio (Port->Atu, 0x4000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3MapMmio (Port->Link, 4096);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3MapMmio (Port->Config, Port->ConfigSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (UINT32 I = 0; I < Port->RangeCount; I++) {
    Status = K3MapMmio ((UINTN)Port->Ranges[I].Cpu, (UINTN)Port->Ranges[I].Size);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  // Quiesce training while preparing the controller and PHY.
  MmioAndThenOr32 (Port->Control, ~LTSSM, HOLD_PHY);
  MmioOr32 (Port->Control + 4, BIT0);
  (VOID)MmioRead32 (Port->Control + 4);
  MicroSecondDelay (2000);
  MmioAnd32 (Port->Control + 4, ~BIT0);
  MmioOr32 (Port->Control, CLK_RESET_BITS | AUX_POWER);
  MmioAnd32 (Port->Control, ~HOLD_PHY);
  // K3 host mode is checked from the actual DBI header, not by applying
  // K1's contradictory device-type bit recipe to the shared wrapper.
  if ((MmioRead8 (Port->Dbi + 0xe) & 0x7f) != 1) {
    Status = EFI_UNSUPPORTED;
    goto Fail;
  }

  Status = Phy->Pcie (Fdt, Child, Port->Lanes);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  // Assert endpoint reset after controller reset and reference-clock setup.
  // The earlier assertion leaves RTL8127 DMA broken after a warm reboot.
  MmioAndThenOr32 (Port->Control + 4, ~PERST_OUT, IGNORE_PERST | PERST_OE);
  (VOID)MmioRead32 (Port->Control + 4);
  // PCIe fundamental reset stays asserted at least 100ms after stable supply.
  MicroSecondDelay (100000);
  MmioOr32 (Port->Dbi + 0x8bc, BIT0);
  MmioWrite32 (Port->Dbi, 0x0002201f);
  MmioAndThenOr32 (Port->Dbi + 8, 0xff, 0x06040000);
  MmioWrite32 (Port->Dbi2 + 0x10, 0);
  MmioWrite32 (Port->Dbi2 + 0x14, 0);
  UINT32  Mode = Port->Lanes == 1 ? 1 : Port->Lanes == 2 ? 3 : Port->Lanes == 4 ? 7 : 15;

  MmioAndThenOr32 (Port->Dbi + 0x710, ~(0x3f0000U | BIT7), (Mode << 16) | BIT5);
  MmioAndThenOr32 (Port->Dbi + 0x80c, ~0x1f00U, Port->Lanes << 8);
  MmioAndThenOr32 (Port->Dbi + 0x7c, ~0x3ffU, (Port->Lanes << 4) | Port->MaxSpeed);
  MmioAndThenOr32 (Port->Dbi + 0xa0, ~0xfU, Port->MaxSpeed);
  MmioAndThenOr32 (Port->Dbi + 0x8a8, ~0x00ffff00U, BIT15);
  MmioAnd32 (Port->Dbi + 0x8bc, ~BIT0);
  Value = Port->BusMin | ((Port->BusMin + 1) << 8) | (Port->BusMax << 16);
  MmioAndThenOr32 (Port->Dbi + 0x18, 0xff000000, Value);
  MmioWrite8 (Port->Dbi + 0xc, 16); // PCI cache-line units are DWORDs: 64 / 4.
  MmioWrite16 (Port->Dbi + 4, 7);   // Real byte enables avoid W1C status side effects.
  for (UINT32 I = 0; I < Port->RangeCount; I++) {
    SPACEMIT_PCIE_RANGE  *R = &Port->Ranges[I];
    Status = SpacemitPcieProgramWindow (
               Port,
               I + 1,
               ((R->Flags >> 24) & 3) == 1 ? 2 : 0,
               R->Cpu,
               R->Pci,
               R->Size
               );
    if (EFI_ERROR (Status)) {
      goto Fail;
    }
  }

  MmioOr32 (Port->Control + 4, PERST_OUT | PERST_OE);
  MemoryFence ();
  MmioOr32 (Port->Control, LTSSM);
  MmioOr32 (Port->Dbi + 0x80c, BIT17);
  UINT32   Link;
  BOOLEAN  Active = WaitL0 (Port, &Link);
  UINT32   State  = (Link & LTSSM_MASK) >> 6;

  if (!Active && ((Link & LINK_UP_BITS) == LINK_UP_BITS) &&
      (((State >= 0xd) && (State <= 0x10)) ||
       ((State >= 0x20) && (State <= 0x23))))
  {
    // Link-up indications can persist in Recovery while config reads fail.
    // No PCI consumers exist yet. Retry the observed endpoint PERST boundary
    // once, keeping the controller, shared PHY and clock configuration intact.
    DEBUG ((
      DEBUG_WARN,
      "K3 PCI segment %u stuck in Recovery: %08x; retry PERST\n",
      Port->Segment,
      Link
      ));
    MmioAnd32 (Port->Control + 4, ~PERST_OUT);
    (VOID)MmioRead32 (Port->Control + 4);
    MicroSecondDelay (100000);
    MmioOr32 (Port->Control + 4, PERST_OUT);
    (VOID)MmioRead32 (Port->Control + 4);
    Active = WaitL0 (Port, &Link);
  }

  if (Active) {
    Port->Ready = TRUE;
    DEBUG ((
      DEBUG_INFO,
      "K3 PCI segment %u link L0: %08x\n",
      Port->Segment,
      Link
      ));
    return EFI_SUCCESS;
  }

  DEBUG ((
    DEBUG_WARN,
    "K3 PCI segment %u training timeout: %08x\n",
    Port->Segment,
    Link
    ));
  Status = EFI_TIMEOUT;
Fail:
  Park (Port);
  return Status;
}

EFI_STATUS EFIAPI
K3PciEntry (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  CONST EFI_GUID               Guid = RISCV_SEC_HANDOFF_HOB_GUID;
  EFI_HOB_GUID_TYPE            *Hob = GetFirstGuidHob (&Guid);
  RISCV_SEC_HANDOFF_DATA       Handoff;
  SPACEMIT_COMBO_PHY_PROTOCOL  *Phy;
  SPACEMIT_PCIE_PORT           Candidates[SPACEMIT_PCIE_MAX_PORTS];
  UINTN                        Count;
  EFI_STATUS                   Status, LastError = EFI_NOT_FOUND;

  if (!Hob || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (Handoff))) {
    return EFI_NOT_FOUND;
  }

  CopyMem (&Handoff, GET_GUID_HOB_DATA (Hob), sizeof (Handoff));
  VOID  *Fdt = Handoff.FdtPointer;

  if (!Fdt || FdtCheckHeader (Fdt) || (FdtTotalSize (Fdt) > SIZE_1MB)) {
    return EFI_COMPROMISED_DATA;
  }

  Status = gBS->LocateProtocol (&gSpacemitComboPhyProtocolGuid, NULL, (VOID **)&Phy);
  if (EFI_ERROR (Status) || !Phy || (Phy->Revision < 1) ||
      (Phy->Revision > SPACEMIT_COMBO_PHY_REVISION) || !Phy->Pcie)
  {
    return EFI_NOT_READY;
  }

  Status = SpacemitPcieParse (Fdt, Candidates, ARRAY_SIZE (Candidates), &Count);
  if (EFI_ERROR (Status) || !Count) {
    return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  }

  Status = EnableParent (Fdt);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mCount = 0;
  for (UINTN I = 0; I < Count; I++) {
    Status = Start (Fdt, &Candidates[I], Phy);
    if (EFI_ERROR (Status)) {
      LastError = Status;
      DEBUG ((DEBUG_WARN, "K3 PCI segment %u start failed: %r\n", Candidates[I].Segment, Status));
      continue;
    }

    mPorts[mCount++] = Candidates[I];
  }

  if (!mCount) {
    if (!mParentAlreadyEnabled) {
      MmioAnd32 (mParentRegister, ~BIT5);
    }

    return LastError;
  }

  static SPACEMIT_PCI_CONTROLLER_PROTOCOL  Protocol;

  Protocol = (SPACEMIT_PCI_CONTROLLER_PROTOCOL) {
    1, mCount, mPorts, Config
  };
  EFI_HANDLE  Handle = NULL;

  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &gSpacemitPciControllerProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &Protocol
                  );
  if (EFI_ERROR (Status)) {
    for (UINTN I = 0; I < mCount; I++) {
      Park (&mPorts[I]);
    }

    mCount = 0;
    if (!mParentAlreadyEnabled) {
      MmioAnd32 (mParentRegister, ~BIT5);
    }
  }

  return Status;
}
