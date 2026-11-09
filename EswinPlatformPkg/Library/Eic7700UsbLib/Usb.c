/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtBoardInfoLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/Eic7700UsbLib.h>
#include <Library/TimerLib.h>

// IDs in the EIC7700 clock and reset DT bindings.
#define CLK_HSP_CFG          171
#define CLK_HSP_ACLK         186
#define CLK_HSP_PHY_REF      187
#define HSP_CLK_USB0          15
#define RESET_USB0_VAUX       84
#define HSP_RESET_USB0        2
#define HSP_RESET_USB0_PHY    4

// SYSCRG and HSP register fields from the EIC7700 TRM.
#define HSP_ACLK_CTRL        0x148
#define HSP_CFG_CLK_CTRL     0x14c
#define HSP_RESET_CTRL       0x11c
#define HSP_PHY_REF_CTRL     0x154
#define USB0_VAUX_BIT        15
#define HSP_USB0_BUS         0x800
#define HSP_USB_STRIDE       0x100
#define HSP_USB_AXI_LP       0x08
#define HSP_USB_REF_CLK      0x3c
#define HSP_USB_RESET_N      BIT24
#define HSP_USB_PHY_RESET    BIT25
#define HSP_USB_CLOCK_EN     BIT28
#define HSP_USB_FILTER_EN    BIT0
#define HSP_USB_MASTER_EN    BIT9
#define HSP_USB_SLAVE_EN     BIT16
#define HSP_USB_LP_REQUESTS  (BIT0 | BIT16)
#define USB_REF_MASK        0x3f
#define USB_REF_24MHZ        0x2a

// Synopsys DWC3 global registers and xHCI register block.
#define GCTL                 0xc110
#define GUCTL1               0xc11c
#define GSNPSID              0xc120
#define GUSB2PHYCFG          0xc200
#define GUSB3PIPECTL         0xc2c0
#define GCTL_CORE_RESET      BIT11
#define GCTL_PORT_MASK       (BIT12 | BIT13)
#define GCTL_HOST            BIT12
#define GCTL_SCALE_DOWN      (BIT4 | BIT5)
#define GCTL_DISABLE_SCRAMBLE BIT3
#define PHY_SOFT_RESET       BIT31
#define USB2_SUSPEND_PHY     BIT6
#define USB2_ENABLE_SLEEP    BIT8
#define USB2_FREE_CLOCK      BIT30
#define USB2_16BIT           BIT3
#define USB2_TURNAROUND      (0xfU << 10)
#define USB2_TURNAROUND_16   (5U << 10)
#define USB3_SUSPEND_PHY     BIT17
#define PARKMODE_DISABLE_SS BIT17
#define XHCI_RUN            BIT0
#define XHCI_HALTED         BIT0

STATIC BOOLEAN
String (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name, CONST CHAR8 *Value)
{
  CONST CHAR8  *Text;
  INT32        Length;

  Text = FdtGetProp (Fdt, Node, Name, &Length);
  return Text && (Length == AsciiStrSize (Value)) && !AsciiStrCmp (Text, Value);
}

STATIC EFI_STATUS
Provider (CONST VOID *Fdt, CONST UINT32 *Spec, UINT32 Id,
          CONST CHAR8 *Compatible, UINTN Minimum, UINTN *Base)
{
  INT32  Node;

  if (Fdt32ToCpu (Spec[1]) != Id) {
    return EFI_UNSUPPORTED;
  }
  Node = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Spec[0]));
  if (!FdtNodeEnabled (Fdt, Node) ||
      (FdtStringListSearch (Fdt, Node, "compatible", Compatible) < 0))
  {
    return EFI_UNSUPPORTED;
  }
  return FdtGetMmio (Fdt, Node, 0, Minimum, Base, NULL);
}

STATIC EFI_STATUS
Wait (UINTN Register, UINT32 Mask, UINT32 Expected, UINTN TimeoutUs)
{
  UINTN  Elapsed;

  for (Elapsed = 0; Elapsed <= TimeoutUs; Elapsed += 10) {
    if ((MmioRead32 (Register) & Mask) == Expected) {
      return EFI_SUCCESS;
    }
    if (Elapsed != TimeoutUs) {
      MicroSecondDelay (10);
    }
  }
  return EFI_TIMEOUT;
}

EFI_STATUS
Eic7700UsbInitialize (CONST VOID *Fdt, INT32 Node, BOOLEAN Device, UINTN *Core)
{
  CONST UINT32  *Clocks, *Resets, *Syscon, *Reference, *Parents;
  INT32         Length, ClockNode, Oscillator, HspNode, HspClockNode;
  UINTN         Base, Clock, Reset, Hsp, ProviderBase, HspBytes, Bus, Port;
  UINT32        Value, OpOffset;
  EFI_STATUS    Status;

  if (!String (Fdt, Node, "phy_type", "utmi")) {
    return EFI_UNSUPPORTED;
  }
  Status = FdtGetMmio (Fdt, Node, 0, 0x10000, &Base, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Syscon = FdtGetProp (Fdt, Node, "eswin,hsp-sp-csr", &Length);
  if (!Syscon || (Length != 3 * sizeof (UINT32))) {
    return EFI_COMPROMISED_DATA;
  }
  HspNode = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Syscon[0]));
  if (!FdtNodeEnabled (Fdt, HspNode)) {
    return EFI_UNSUPPORTED;
  }
  Bus = Fdt32ToCpu (Syscon[1]);
  if (((Bus != HSP_USB0_BUS) && (Bus != HSP_USB0_BUS + HSP_USB_STRIDE)) ||
      (Fdt32ToCpu (Syscon[2]) != Bus + HSP_USB_AXI_LP))
  {
    return EFI_UNSUPPORTED;
  }
  Port = (Bus - HSP_USB0_BUS) / HSP_USB_STRIDE;
  Status = FdtGetMmio (Fdt, HspNode, 0, Bus + HSP_USB_REF_CLK + 4, &Hsp, &HspBytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Clocks = FdtGetProp (Fdt, Node, "clocks", &Length);
  if (!Clocks || (Length != 6 * sizeof (UINT32)) ||
      (FdtStringListSearch (Fdt, Node, "clock-names", "aclk") != 0) ||
      (FdtStringListSearch (Fdt, Node, "clock-names", "cfg") != 1) ||
      (FdtStringListSearch (Fdt, Node, "clock-names", "usb_en") != 2))
  {
    return EFI_COMPROMISED_DATA;
  }
  Status = Provider (Fdt, Clocks, CLK_HSP_ACLK, "eswin,eic7700-clock", HSP_CFG_CLK_CTRL + 4, &Clock);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Provider (Fdt, Clocks + 2, CLK_HSP_CFG, "eswin,eic7700-clock", HSP_CFG_CLK_CTRL + 4, &ProviderBase);
  if (EFI_ERROR (Status) || (ProviderBase != Clock)) {
    return EFI_UNSUPPORTED;
  }
  Status = Provider (Fdt, Clocks + 4, HSP_CLK_USB0 + Port, "eswin,eic7700-hspcrg", HspBytes, &ProviderBase);
  if (EFI_ERROR (Status) || (ProviderBase != Hsp)) {
    return EFI_UNSUPPORTED;
  }

  ClockNode = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Clocks[0]));
  Reference = FdtGetProp (Fdt, ClockNode, "clocks", &Length);
  if (!Reference || (Length != sizeof (UINT32))) {
    return EFI_COMPROMISED_DATA;
  }
  Oscillator = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Reference));
  Reference  = FdtGetProp (Fdt, Oscillator, "clock-frequency", &Length);
  if (!Reference || (Length != sizeof (UINT32)) || (Fdt32ToCpu (*Reference) != 24000000)) {
    return EFI_UNSUPPORTED;
  }

  HspClockNode = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Clocks[4]));
  Parents = FdtGetProp (Fdt, HspClockNode, "clocks", &Length);
  if (!Parents || (Length != 6 * sizeof (UINT32)) ||
      (FdtStringListSearch (Fdt, HspClockNode, "clock-names", "sata") != 2))
  {
    return EFI_COMPROMISED_DATA;
  }
  Status = Provider (Fdt, Parents + 4, CLK_HSP_PHY_REF, "eswin,eic7700-clock", HSP_PHY_REF_CTRL + 4, &ProviderBase);
  if (EFI_ERROR (Status) || (ProviderBase != Clock)) {
    return EFI_UNSUPPORTED;
  }

  Resets = FdtGetProp (Fdt, Node, "resets", &Length);
  if (!Resets || (Length != 6 * sizeof (UINT32)) ||
      (FdtStringListSearch (Fdt, Node, "reset-names", "vaux") != 0) ||
      (FdtStringListSearch (Fdt, Node, "reset-names", "usb_rst") != 1) ||
      (FdtStringListSearch (Fdt, Node, "reset-names", "usb_phy") != 2))
  {
    return EFI_COMPROMISED_DATA;
  }
  Status = Provider (Fdt, Resets, RESET_USB0_VAUX + Port, "eswin,eic7700-reset", HSP_RESET_CTRL + 4, &Reset);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Provider (Fdt, Resets + 2, HSP_RESET_USB0 + Port, "eswin,eic7700-hspcrg", HspBytes, &ProviderBase);
  if (EFI_ERROR (Status) || (ProviderBase != Hsp)) {
    return EFI_UNSUPPORTED;
  }
  Status = Provider (Fdt, Resets + 4, HSP_RESET_USB0_PHY + Port, "eswin,eic7700-hspcrg", HspBytes, &ProviderBase);
  if (EFI_ERROR (Status) || (ProviderBase != Hsp)) {
    return EFI_UNSUPPORTED;
  }

  Status = FdtMapMmio (Clock, HSP_PHY_REF_CTRL + 4);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (Reset, HSP_RESET_CTRL + 4);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (Hsp, HspBytes);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (Base, 0x10000);
  if (EFI_ERROR (Status)) { return Status; }

  MmioOr32 (Clock + HSP_ACLK_CTRL, BIT31);
  MmioOr32 (Clock + HSP_CFG_CLK_CTRL, BIT31);
  MmioOr32 (Clock + HSP_PHY_REF_CTRL, BIT31);
  MmioAnd32 (Reset + HSP_RESET_CTRL, ~((UINT32)BIT0 << (USB0_VAUX_BIT + Port)));
  MmioAndThenOr32 (Hsp + Bus, ~((UINT32)HSP_USB_RESET_N), HSP_USB_PHY_RESET);
  MicroSecondDelay (2);
  MmioOr32 (Reset + HSP_RESET_CTRL, (UINT32)BIT0 << (USB0_VAUX_BIT + Port));
  MmioAndThenOr32 (Hsp + Bus, ~((UINT32)HSP_USB_PHY_RESET), HSP_USB_RESET_N);
  // The reset reference-clock encoding is invalid on EIC7700.
  MmioAndThenOr32 (Hsp + Bus + HSP_USB_REF_CLK, ~((UINT32)USB_REF_MASK), USB_REF_24MHZ);
  MmioOr32 (Hsp + Bus, HSP_USB_CLOCK_EN | HSP_USB_FILTER_EN | HSP_USB_MASTER_EN | HSP_USB_SLAVE_EN);
  MmioOr32 (Hsp + Bus + HSP_USB_AXI_LP, HSP_USB_LP_REQUESTS);

  Value = MmioRead32 (Base + GSNPSID);
  if ((Value & 0xffff0000) != 0x55330000) {
    Status = EFI_UNSUPPORTED;
    goto AssertReset;
  }
  // The platform resets have reset the PHY and controller. Select the
  // requested role before the xHCI or device driver starts transfers.
  MmioAnd32 (Base + GUSB3PIPECTL, ~((UINT32)(PHY_SOFT_RESET | USB3_SUSPEND_PHY)));
  MmioAndThenOr32 (Base + GUSB2PHYCFG,
                  ~((UINT32)(PHY_SOFT_RESET | USB2_SUSPEND_PHY | USB2_ENABLE_SLEEP | USB2_FREE_CLOCK | USB2_TURNAROUND)),
                  USB2_16BIT | USB2_TURNAROUND_16);
  MmioAndThenOr32 (Base + GCTL,
                  ~((UINT32)(GCTL_CORE_RESET | GCTL_PORT_MASK | GCTL_SCALE_DOWN | GCTL_DISABLE_SCRAMBLE)), Device ? (2U << 12) : GCTL_HOST);
  if (FdtGetProp (Fdt, Node, "snps,parkmode-disable-ss-quirk", &Length)) {
    MmioOr32 (Base + GUCTL1, PARKMODE_DISABLE_SS);
  }

  *Core = Base;
  if (Device) {
    return EFI_SUCCESS;
  }

  OpOffset = MmioRead32 (Base) & 0xff;
  if ((OpOffset < 0x20) || (OpOffset > 0x100)) {
    Status = EFI_DEVICE_ERROR;
    goto AssertReset;
  }
  MmioAnd32 (Base + OpOffset, ~((UINT32)XHCI_RUN));
  Status = Wait (Base + OpOffset + 4, XHCI_HALTED, XHCI_HALTED, 100000);
  if (EFI_ERROR (Status)) {
    goto AssertReset;
  }

  return EFI_SUCCESS;
AssertReset:
  MmioAndThenOr32 (Hsp + Bus, ~((UINT32)HSP_USB_RESET_N), HSP_USB_PHY_RESET);
  MmioAnd32 (Reset + HSP_RESET_CTRL, ~((UINT32)BIT0 << (USB0_VAUX_BIT + Port)));
  return Status;
}
