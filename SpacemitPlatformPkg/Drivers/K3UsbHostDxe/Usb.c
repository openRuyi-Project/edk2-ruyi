/** SPDX-License-Identifier: BSD-2-Clause-Patent */

/* K3 USB PHY/global setup based on the pinned BSD vendor UEFI reference.
 * Copyright 2017, 2020 NXP
 * Copyright 2025, SpacemiT Co., Ltd. All rights reserved.
 * Resources and reset IDs are checked against the standard K3 DT bindings.
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
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/ComboPhy.h>

#define USB_CLOCK_CONTROL  0x5c
#define XHCI_BYTES         0xc100U
#define GCTL               0xc110
#define GSNPSID            0xc120
#define GHWPARAMS0         0xc140
#define GHWPARAMS1         0xc144
#define GHWPARAMS3         0xc14c
#define GUCTL1             0xc11c
#define GUSB2PHYCFG        0xc200
#define GUSB3PIPECTL       0xc2c0
#define DCTL               0xc704
#define GCTL_HOST          (1U << 12)
#define GCTL_MODE_MASK     (3U << 12)
#define USBSTS_HALT        BIT0
#define USBSTS_CNR         BIT11
#define FORCE_USB2_CLOCK   BIT26
#define USB2_PLL_READY     BIT0

typedef struct {
  INT32         Node;
  UINTN         Base, Phy, Apmu;
  UINT32        Gate, Resets;
  BOOLEAN       SuperSpeed, Attempted, Owned, Ready, Quarantined;
  EFI_STATUS    Status;
  EFI_HANDLE    Handle;
} USB_PORT;
STATIC USB_PORT                     mPorts[4];
STATIC CONST VOID                   *mFdt;
STATIC SPACEMIT_COMBO_PHY_PROTOCOL  *mCombo;
STATIC EFI_EVENT                    mNotifyEvent;
STATIC VOID                         *mRegistration;
STATIC BOOLEAN                      mInitializing, mPending;

STATIC BOOLEAN
Cell (
  INT32        Node,
  CONST CHAR8  *Name,
  UINT32       Value
  )
{
  INT32         Length;
  CONST UINT32  *P = FdtGetProp (mFdt, Node, Name, &Length);

  return P && Length == 4 && Fdt32ToCpu (*P) == Value;
}

STATIC BOOLEAN
String (
  INT32        Node,
  CONST CHAR8  *Name,
  CONST CHAR8  *Value
  )
{
  INT32        Length;
  CONST CHAR8  *P = FdtGetProp (mFdt, Node, Name, &Length);

  return P && Length == (INT32)AsciiStrLen (Value) + 1 && !AsciiStrCmp (P, Value);
}

STATIC EFI_STATUS
Wait (
  UINTN   Address,
  UINT32  Mask,
  UINT32  Expected,
  UINTN   TimeoutUs
  )
{
  for (UINTN Elapsed = 0; Elapsed <= TimeoutUs; Elapsed += 500) {
    if ((MmioRead32 (Address) & Mask) == Expected) {
      return EFI_SUCCESS;
    }

    if (Elapsed < TimeoutUs) {
      MicroSecondDelay (500);
    }
  }

  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
Parse (
  INT32     Node,
  USB_PORT  *Port,
  UINTN     Id
  )
{
  static CONST UINT32  ClockIds[]    = { 30, 32, 33, 34 };
  static CONST UINT32  ResetIds[][3] = {
    { 16, 17, 18 }, { 52, 62, 63 }, { 64, 65, 66 }, { 72, 73, 74 }
  };
  static CONST UINTN   PhyBases[] = { 0xc0a20000, 0x81500000, 0x81800000, 0x81b00000 };
  static CONST UINT32  Gates[]    = { BIT1, BIT8, BIT12, BIT16 };
  static CONST UINT32  Resets[]   = {
    BIT0 | BIT2 | BIT3,    BIT9 | BIT10 | BIT11,
    BIT13 | BIT14 | BIT15, BIT17 | BIT18 | BIT19
  };
  INT32                Length, Provider, Phy;
  UINTN                Bytes, Base;
  EFI_STATUS           Status;

  Port->Node   = Node;
  Port->Gate   = Gates[Id];
  Port->Resets = Resets[Id];
  if (!String (Node, "phy_type", "utmi")) {
    return EFI_UNSUPPORTED;
  }

  CONST VOID  *Speed = FdtGetProp (mFdt, Node, "maximum-speed", &Length);

  if (Speed && !String (Node, "maximum-speed", "high-speed") &&
      !String (Node, "maximum-speed", "super-speed"))
  {
    return EFI_UNSUPPORTED;
  }

  Port->SuperSpeed = !String (Node, "maximum-speed", "high-speed");
  if ((Id == 0) && Port->SuperSpeed) {
    return EFI_UNSUPPORTED;
  }

  CONST UINT32  *Clocks = FdtGetProp (mFdt, Node, "clocks", &Length);

  if (!Clocks || (Length != 8) || (Fdt32ToCpu (Clocks[1]) != ClockIds[Id]) ||
      (FdtStringListSearch (mFdt, Node, "clock-names", "usbdrd30") != 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  Provider = FdtNodeOffsetByPhandle (mFdt, Fdt32ToCpu (Clocks[0]));
  if (!K3FdtEnabled (mFdt, Provider) ||
      (FdtStringListSearch (mFdt, Provider, "compatible", "spacemit,k3-syscon-apmu") < 0) ||
      !Cell (Provider, "#clock-cells", 1) || !Cell (Provider, "#reset-cells", 1))
  {
    return EFI_COMPROMISED_DATA;
  }

  Status = K3FdtRegister (mFdt, Provider, 0, 0x60, &Port->Apmu, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CONST UINT32  *R = FdtGetProp (mFdt, Node, "resets", &Length);

  if (!R || (Length != 24)) {
    return EFI_COMPROMISED_DATA;
  }

  static CONST CHAR8  *Names[] = { "ahb", "vcc", "phy" };

  for (UINTN I = 0; I < 3; I++) {
    if ((R[I * 2] != Clocks[0]) || (Fdt32ToCpu (R[I * 2 + 1]) != ResetIds[Id][I]) ||
        (FdtStringListSearch (mFdt, Node, "reset-names", Names[I]) != (INT32)I))
    {
      return EFI_COMPROMISED_DATA;
    }
  }

  // The USB2 PLL recipe selects a 24MHz reference; validate its DT source.
  INT32         ReferenceIndex = FdtStringListSearch (mFdt, Provider, "clock-names", "vctcxo_24m");
  CONST UINT32  *References    = FdtGetProp (mFdt, Provider, "clocks", &Length);

  if (!References || (Length <= 0) || Length % 4 || (ReferenceIndex < 0) ||
      (ReferenceIndex >= Length / 4))
  {
    return EFI_COMPROMISED_DATA;
  }

  INT32  Reference = FdtNodeOffsetByPhandle (mFdt, Fdt32ToCpu (References[ReferenceIndex]));

  if (!K3FdtEnabled (mFdt, Reference) ||
      (FdtStringListSearch (mFdt, Reference, "compatible", "fixed-clock") < 0) ||
      !Cell (Reference, "#clock-cells", 0) || !Cell (Reference, "clock-frequency", 24000000))
  {
    return EFI_UNSUPPORTED;
  }

  CONST UINT32  *Phys = FdtGetProp (mFdt, Node, "phys", &Length);

  if (!Phys || (Length != (Port->SuperSpeed ? 16 : 4)) ||
      (FdtStringListSearch (mFdt, Node, "phy-names", "usb2-phy") != 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  Phy = FdtNodeOffsetByPhandle (mFdt, Fdt32ToCpu (Phys[0]));
  if (!K3FdtEnabled (mFdt, Phy) || !Cell (Phy, "#phy-cells", 0) ||
      (FdtStringListSearch (mFdt, Phy, "compatible", "spacemit,k3-usb2-phy") < 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  CONST UINT32  *PhyClock = FdtGetProp (mFdt, Phy, "clocks", &Length);

  if (!PhyClock || (Length != 8) || (PhyClock[0] != Clocks[0]) || (PhyClock[1] != Clocks[1])) {
    return EFI_COMPROMISED_DATA;
  }

  Status = K3FdtRegister (mFdt, Phy, 0, 0x200, &Port->Phy, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Port->Phy != PhyBases[Id]) {
    return EFI_UNSUPPORTED;
  }

  if (Port->SuperSpeed) {
    INT32  Combo = FdtNodeOffsetByPhandle (mFdt, Fdt32ToCpu (Phys[1]));
    if (!K3FdtEnabled (mFdt, Combo) || !Cell (Combo, "#phy-cells", 2) ||
        (FdtStringListSearch (mFdt, Combo, "compatible", "spacemit,k3-combo-phy") < 0) ||
        (FdtStringListSearch (mFdt, Node, "phy-names", "usb3-phy") != 1) ||
        (Fdt32ToCpu (Phys[2]) != Id + 1) || (Fdt32ToCpu (Phys[3]) != 4))
    {
      return EFI_COMPROMISED_DATA;
    }
  }

  if (FdtGetProp (mFdt, Node, "vbus-supply", &Length)) {
    Status = K3RequireFixedSupply (mFdt, Node, "vbus-supply", 5000000);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  // Preserve and validate board-described powered USB hub dependencies.
  for (INT32 Child = FdtFirstSubnode (mFdt, Node); Child >= 0; Child = FdtNextSubnode (mFdt, Child)) {
    if (K3FdtEnabled (mFdt, Child) && FdtGetProp (mFdt, Child, "vdd-supply", &Length)) {
      Status = K3RequireFixedSupply (mFdt, Child, "vdd-supply", 3300000);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  Status = K3FdtRegister (mFdt, Node, 0, 0x10000, &Base, &Bytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Port->Base = Base;
  Status     = K3MapMmio (Base, Bytes);
  if (!EFI_ERROR (Status)) {
    Status = K3MapMmio (Port->Phy, 0x200);
  }

  if (!EFI_ERROR (Status)) {
    Status = K3MapMmio (Port->Apmu, 0x60);
  }

  return Status;
}

STATIC EFI_STATUS
Capabilities (
  USB_PORT  *P,
  UINTN     *Operational
  )
{
  UINT32  Cap = MmioRead32 (P->Base);
  UINTN   Length = Cap & 0xff;
  UINT32  Slots = MmioRead32 (P->Base + 4), Intrs = (Slots >> 8) & 0x7ff;
  UINT32  Runtime   = MmioRead32 (P->Base + 0x18) & ~0x1fU;
  UINT32  Doorbells = MmioRead32 (P->Base + 0x14) & ~3U;

  if ((Length < 0x20) || (Length > 0x100) || Length % 4 ||
      ((Cap >> 16) < 0x100) || ((Cap >> 16) > 0x200) ||
      !(Slots & 0xff) || !(Slots >> 24) || !Intrs ||
      (Runtime < Length) || (Runtime + 0x20ULL + Intrs * 32ULL > XHCI_BYTES) ||
      (Doorbells < Length) || (Doorbells + ((Slots & 0xff) + 1ULL) * 4 > XHCI_BYTES))
  {
    return EFI_COMPROMISED_DATA;
  }

  if (!(MmioRead32 (P->Base + 0x10) & BIT0)) {
    return EFI_UNSUPPORTED;
  }

  *Operational = P->Base + Length;
  return EFI_SUCCESS;
}

STATIC VOID
Park (
  USB_PORT  *P
  )
{
  if (!P->Owned) {
    return;
  }

  // Registration has not succeeded, so no HCD/DMA resources were published.
  MmioAnd32 (P->Apmu + USB_CLOCK_CONTROL, ~P->Resets);
  if (MmioRead32 (P->Apmu + USB_CLOCK_CONTROL) & P->Resets) {
    P->Quarantined = TRUE;
    return;
  }

  MmioAnd32 (P->Apmu + USB_CLOCK_CONTROL, ~P->Gate);
  if (MmioRead32 (P->Apmu + USB_CLOCK_CONTROL) & P->Gate) {
    P->Quarantined = TRUE;
  }
}

STATIC EFI_STATUS
Initialize (
  USB_PORT  *P
  )
{
  UINT32      Mask = P->Gate | P->Resets, Revision;
  UINTN       Operational;
  EFI_STATUS  Status;

  if (P->SuperSpeed && !mCombo) {
    return EFI_NOT_READY;
  }

  if ((MmioRead32 (P->Apmu + USB_CLOCK_CONTROL) & Mask) == Mask) {
    // A warm active controller belongs to its current owner, even if DT
    // requests host mode. Never reset it or replace its DMA schedule.
    if ((MmioRead32 (P->Base + GSNPSID) & 0xffff0000) != 0x55330000) {
      return EFI_NOT_READY;
    }

    Status = Capabilities (P, &Operational);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((MmioRead32 (Operational) & BIT0) || !(MmioRead32 (Operational + 4) & USBSTS_HALT) ||
        (MmioRead32 (P->Base + DCTL) & BIT31))
    {
      return EFI_ACCESS_DENIED;
    }
  }

  INT32  Length;

  if (FdtGetProp (mFdt, P->Node, "pinctrl-0", &Length)) {
    Status = K3ApplyPins (mFdt, P->Node);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  P->Owned = TRUE;
  MmioAndThenOr32 (P->Apmu + USB_CLOCK_CONTROL, ~P->Resets, P->Gate);
  if ((MmioRead32 (P->Apmu + USB_CLOCK_CONTROL) & Mask) != P->Gate) {
    return EFI_DEVICE_ERROR;
  }

  MmioOr32 (P->Apmu + USB_CLOCK_CONTROL, P->Resets);
  if ((MmioRead32 (P->Apmu + USB_CLOCK_CONTROL) & Mask) != Mask) {
    return EFI_DEVICE_ERROR;
  }

  MicroSecondDelay (200);
  // 24MHz PLL divider; Linux K3 reference bounds the ready wait to 50ms.
  MmioWrite32 (P->Phy + 0x98, 0xbec4);
  Status = Wait (P->Phy + 4, USB2_PLL_READY, USB2_PLL_READY, 50000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (P->Phy + 4, 0x60ef);
  MmioWrite32 (P->Phy + 0x34, 0x1c);
  MmioAndThenOr32 (P->Phy + 0xa4, ~0xfU, 0xc | BIT4);
  MmioOr32 (P->Phy + 0x10, BIT2);
  if (P->SuperSpeed) {
    Status = mCombo->Usb3 (mFdt, P->Node);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Revision = MmioRead32 (P->Base + GSNPSID);
  if ((Revision & 0xffff0000) != 0x55330000) {
    return EFI_NOT_FOUND;
  }

  UINT32  Mode = MmioRead32 (P->Base + GHWPARAMS0) & 3;

  if ((Mode != 1) && (Mode != 2)) {
    return EFI_UNSUPPORTED;
  }

  if (P->SuperSpeed && !(MmioRead32 (P->Base + GHWPARAMS3) & 3)) {
    return EFI_UNSUPPORTED;
  }

  UINT32  U2Clear = BIT3 | (0xfU << 10) | BIT31;

  if (FdtGetProp (mFdt, P->Node, "snps,dis_enblslpm_quirk", &Length)) {
    U2Clear |= BIT8;
  }

  if (FdtGetProp (mFdt, P->Node, "snps,dis_u2_susphy_quirk", &Length)) {
    U2Clear |= BIT6;
  }

  // ENBLSLPM is bit 8; bit 0 belongs to TOUTCAL. Do not copy the vendor typo.
  MmioAndThenOr32 (P->Base + GUSB2PHYCFG, ~U2Clear, (9U << 10) | 7);
  UINT32  U3Clear = BIT31;

  if (FdtGetProp (mFdt, P->Node, "snps,dis_u3_susphy_quirk", &Length)) {
    U3Clear |= BIT17;
  }

  if (FdtGetProp (mFdt, P->Node, "snps,dis-del-phy-power-chg-quirk", &Length)) {
    U3Clear |= BIT18;
  }

  MmioAnd32 (P->Base + GUSB3PIPECTL, ~U3Clear);
  UINT32  Uctl = 0;

  if (FdtGetProp (mFdt, P->Node, "snps,dis-tx-ipgap-linecheck-quirk", &Length)) {
    Uctl |= BIT28;
  }

  if (FdtGetProp (mFdt, P->Node, "snps,parkmode-disable-ss-quirk", &Length)) {
    Uctl |= BIT17;
  }

  if (!P->SuperSpeed) {
    Uctl |= FORCE_USB2_CLOCK;
  }

  MmioAndThenOr32 (P->Base + GUCTL1, ~FORCE_USB2_CLOCK, Uctl);
  UINT32  Control = MmioRead32 (P->Base + GCTL) & ~(BIT3 | BIT4 | BIT5 | BIT11 | GCTL_MODE_MASK);

  if (((MmioRead32 (P->Base + GHWPARAMS1) >> 24) & 3) == 1) {
    Control &= ~BIT0;
  }

  if ((Revision & 0xffff) < 0x190a) {
    Control |= BIT16;
  }

  MmioWrite32 (P->Base + GCTL, Control | GCTL_HOST);
  Status = Capabilities (P, &Operational);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Wait (Operational + 4, USBSTS_HALT | USBSTS_CNR, USBSTS_HALT, 50000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return RegisterNonDiscoverableMmioDevice (
           NonDiscoverableDeviceTypeXhci,
           NonDiscoverableDeviceDmaTypeNonCoherent,
           NULL,
           &P->Handle,
           1,
           P->Base,
           (UINTN)XHCI_BYTES
           );
}

STATIC VOID
InitializePorts (
  VOID
  )
{
  if (mInitializing) {
    mPending = TRUE;
    return;
  }

  mInitializing = TRUE;
  do {
    mPending = FALSE;
    for (UINTN I = 0; I < ARRAY_SIZE (mPorts); I++) {
      USB_PORT  *P = &mPorts[I];
      if ((P->Node < 0) || P->Attempted || EFI_ERROR (P->Status) || (P->SuperSpeed && !mCombo)) {
        continue;
      }

      P->Attempted = TRUE;
      P->Status    = Initialize (P);
      P->Ready     = !EFI_ERROR (P->Status);
      if (!P->Ready) {
        Park (P);
      }

      DEBUG ((
        DEBUG_INFO,
        "K3 USB host %lx %a native registration: %r%s\n",
        P->Base,
        P->SuperSpeed ? "SS" : "HS",
        P->Status,
        P->Quarantined ? L" (quarantined)" : L""
        ));
    }
  } while (mPending);

  mInitializing = FALSE;
  BOOLEAN  Done = TRUE;

  for (UINTN I = 0; I < ARRAY_SIZE (mPorts); I++) {
    if ((mPorts[I].Node >= 0) && !EFI_ERROR (mPorts[I].Status) && !mPorts[I].Attempted) {
      Done = FALSE;
    }
  }

  if (Done && mNotifyEvent) {
    gBS->CloseEvent (mNotifyEvent);
    mNotifyEvent = NULL;
  }
}

STATIC VOID EFIAPI
ComboReady (
  EFI_EVENT  Event,
  VOID       *Context
  )
{
  SPACEMIT_COMBO_PHY_PROTOCOL  *Protocol;
  EFI_STATUS                   Status = gBS->LocateProtocol (&gSpacemitComboPhyProtocolGuid, NULL, (VOID **)&Protocol);

  if (!EFI_ERROR (Status) && Protocol && (Protocol->Revision == SPACEMIT_COMBO_PHY_REVISION) &&
      Protocol->Usb3)
  {
    mCombo = Protocol;
  }

  InitializePorts ();
}

EFI_STATUS EFIAPI
K3UsbHostEntry (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  CONST EFI_GUID          Guid = RISCV_SEC_HANDOFF_HOB_GUID;
  EFI_HOB_GUID_TYPE       *Hob = GetFirstGuidHob (&Guid);
  RISCV_SEC_HANDOFF_DATA  Handoff;

  if (!Hob || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (Handoff))) {
    return EFI_NOT_FOUND;
  }

  CopyMem (&Handoff, GET_GUID_HOB_DATA (Hob), sizeof (Handoff));
  mFdt = Handoff.FdtPointer;
  if (!mFdt || FdtCheckHeader (mFdt) || (FdtTotalSize (mFdt) > SIZE_1MB)) {
    return EFI_COMPROMISED_DATA;
  }

  for (UINTN I = 0; I < ARRAY_SIZE (mPorts); I++) {
    mPorts[I].Node = -1;
  }

  INT32       Node      = -1;
  UINTN       Count     = 0;
  EFI_STATUS  LastError = EFI_NOT_FOUND;

  while ((Node = FdtNodeOffsetByCompatible (mFdt, Node, "spacemit,k3-dwc3")) >= 0) {
    if (!K3FdtEnabled (mFdt, Node) || !String (Node, "dr_mode", "host")) {
      continue;
    }

    UINTN       Base;
    EFI_STATUS  Status = K3FdtRegister (mFdt, Node, 0, 0x10000, &Base, NULL);
    if (EFI_ERROR (Status)) {
      LastError = Status;
      continue;
    }

    UINTN  Id;
    if (Base == 0xc0a00000) {
      Id = 0;
    } else if ((Base >= 0x81400000) && (Base <= 0x81a00000) && ((Base - 0x81400000) % 0x300000 == 0)) {
      Id = 1 + (Base - 0x81400000) / 0x300000;
    } else {
      LastError = EFI_UNSUPPORTED;
      continue;
    }

    if (mPorts[Id].Node >= 0) {
      return EFI_COMPROMISED_DATA;
    }

    mPorts[Id].Status = Parse (Node, &mPorts[Id], Id);
    if (EFI_ERROR (mPorts[Id].Status)) {
      LastError = mPorts[Id].Status;
      DEBUG ((DEBUG_ERROR, "K3 USB host %lx resource validation: %r\n", Base, LastError));
    } else {
      Count++;
    }
  }

  if (!Count) {
    return LastError;
  }

  EFI_STATUS  Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ComboReady, NULL, &mNotifyEvent);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->RegisterProtocolNotify (&gSpacemitComboPhyProtocolGuid, mNotifyEvent, &mRegistration);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mNotifyEvent);
    mNotifyEvent = NULL;
    return Status;
  }

  ComboReady (mNotifyEvent, NULL);
  // Deferred combo readiness must not prevent independent HS controllers.
  for (UINTN I = 0; I < ARRAY_SIZE (mPorts); I++) {
    if (mPorts[I].Ready || ((mPorts[I].Node >= 0) && !mPorts[I].Attempted && !EFI_ERROR (mPorts[I].Status))) {
      return EFI_SUCCESS;
    }

    if (mPorts[I].Attempted) {
      LastError = mPorts[I].Status;
    }
  }

  return LastError;
}
