/** SPDX-License-Identifier: BSD-2-Clause-Patent */

/* K3 offsets/sequence reference: BSD SpacemiT DwEqosSpacemit and K3 CCU.
 * Copyright (c) 2025, Spacemit Corporation. All rights reserved.
 * DT reset/clock wiring is checked against the pinned standard K3 bindings.
 */
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/K3EqosLib.h>
#include <Library/TimerLib.h>

STATIC BOOLEAN
Cell (
  CONST VOID   *Fdt,
  INT32        Node,
  CONST CHAR8  *Name,
  UINT32       Value
  )
{
  INT32         Length;
  CONST UINT32  *P = FdtGetProp (Fdt, Node, Name, &Length);

  return P && Length == 4 && Fdt32ToCpu (*P) == Value;
}

STATIC EFI_STATUS
Delay (
  CONST VOID   *Fdt,
  INT32        Node,
  CONST CHAR8  *Name,
  UINT32       *Code
  )
{
  INT32         Length;
  CONST UINT32  *P = FdtGetProp (Fdt, Node, Name, &Length);

  *Code = 0;
  if (!P) {
    return EFI_SUCCESS;
  }

  if ((Length != 4) || (Fdt32ToCpu (*P) > 2800)) {
    return EFI_COMPROMISED_DATA;
  }

  // K3 fine delay step 36.7ps with the documented 0.9 correction factor.
  *Code = (Fdt32ToCpu (*P) * 100 + (367 * 9) / 2) / (367 * 9);
  return EFI_SUCCESS;
}

EFI_STATUS
K3EqosParse (
  CONST VOID    *Fdt,
  INT32         Node,
  K3_EQOS_PORT  *Port
  )
{
  INT32                Length, Provider, Pll, Mpmu, Phy, Id;
  UINTN                Base, Bytes, Apmu;
  static CONST UINTN   Bases[] = { 0xcac80000, 0xcac82000, 0xcac8e000 };
  static CONST UINT32  Offsets[] = { 0x3e4, 0x3ec, 0x248 };
  static CONST UINT32  BusIds[] = { 66, 70, 74 }, ResetIds[] = { 67, 68, 69 };

  if (!Port || !Fdt || FdtCheckHeader (Fdt) || !K3FdtEnabled (Fdt, Node) ||
      (FdtStringListSearch (Fdt, Node, "compatible", "spacemit,k3-dwmac") < 0))
  {
    return EFI_NOT_FOUND;
  }

  ZeroMem (Port, sizeof (*Port));
  Port->Fdt  = Fdt;
  Port->Node = Node;
  EFI_STATUS  Status = K3FdtRegister (Fdt, Node, 0, 0x2000, &Base, &Bytes);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Id = 0; Id < 3 && Base != Bases[Id]; Id++) {
  }

  if (Id == 3) {
    return EFI_UNSUPPORTED;
  }

  CONST UINT32  *P = FdtGetProp (Fdt, Node, "spacemit,apmu", &Length);

  if (!P || (Length != 12) || (Fdt32ToCpu (P[1]) != Offsets[Id]) ||
      (Fdt32ToCpu (P[2]) != Offsets[Id] + 4))
  {
    return EFI_COMPROMISED_DATA;
  }

  Provider = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*P));
  if (!K3FdtEnabled (Fdt, Provider) ||
      (FdtStringListSearch (Fdt, Provider, "compatible", "spacemit,k3-syscon-apmu") < 0) ||
      !Cell (Fdt, Provider, "#clock-cells", 1) || !Cell (Fdt, Provider, "#reset-cells", 1))
  {
    return EFI_COMPROMISED_DATA;
  }

  Status = K3FdtRegister (Fdt, Provider, 0, 0x3f4, &Apmu, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CONST UINT32  *Clocks = FdtGetProp (Fdt, Node, "clocks", &Length);

  if (!Clocks || (Length != 24)) {
    return EFI_COMPROMISED_DATA;
  }

  static CONST CHAR8  *Names[] = { "stmmaceth", "ptp_ref", "tx" };

  for (INT32 I = 0; I < 3; I++) {
    UINT32  Expected = BusIds[Id] + (I == 0 ? 0 : I + 1);
    if ((FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Clocks[I * 2])) != Provider) ||
        (Fdt32ToCpu (Clocks[I * 2 + 1]) != Expected) ||
        (FdtStringListSearch (Fdt, Node, "clock-names", Names[I]) != I))
    {
      return EFI_COMPROMISED_DATA;
    }
  }

  P = FdtGetProp (Fdt, Node, "resets", &Length);
  if (!P || (Length != 8) || (FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (P[0])) != Provider) ||
      (Fdt32ToCpu (P[1]) != ResetIds[Id]))
  {
    return EFI_COMPROMISED_DATA;
  }

  CONST CHAR8  *Mode = FdtGetProp (Fdt, Node, "phy-mode", &Length);

  if (!Mode || (Length < 6) || (Length > 11) || Mode[Length - 1]) {
    return EFI_COMPROMISED_DATA;
  }

  if (!AsciiStrCmp (Mode, "rgmii")) {
    Port->DelayFlags = 0;
  } else if (!AsciiStrCmp (Mode, "rgmii-id")) {
    Port->DelayFlags = 3;
  } else if (!AsciiStrCmp (Mode, "rgmii-rxid")) {
    Port->DelayFlags = 2;
  } else if (!AsciiStrCmp (Mode, "rgmii-txid")) {
    Port->DelayFlags = 1;
  } else {
    return EFI_UNSUPPORTED;
  }

  P = FdtGetProp (Fdt, Node, "phy-handle", &Length);
  if (!P || (Length != 4)) {
    return EFI_COMPROMISED_DATA;
  }

  Phy = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*P));
  INT32  Mdio = FdtParentOffset (Fdt, Phy);

  if (!K3FdtEnabled (Fdt, Phy) || (FdtParentOffset (Fdt, Mdio) != Node) ||
      !Cell (Fdt, Mdio, "#address-cells", 1) || !Cell (Fdt, Mdio, "#size-cells", 0) ||
      (FdtStringListSearch (Fdt, Phy, "compatible", "ethernet-phy-id001c.c916") < 0))
  {
    return EFI_UNSUPPORTED;
  }

  P = FdtGetProp (Fdt, Phy, "reg", &Length);
  if (!P || (Length != 4) || (Fdt32ToCpu (*P) > 31)) {
    return EFI_COMPROMISED_DATA;
  }

  Port->PhyNode    = Phy;
  Port->PhyAddress = Fdt32ToCpu (*P);
  P                = FdtGetProp (Fdt, Phy, "reset-assert-us", &Length);
  if (!P || (Length != 4) || !Fdt32ToCpu (*P) || (Fdt32ToCpu (*P) > 100000)) {
    return EFI_COMPROMISED_DATA;
  }

  Port->AssertUs = Fdt32ToCpu (*P);
  P              = FdtGetProp (Fdt, Phy, "reset-deassert-us", &Length);
  if (!P || (Length != 4) || !Fdt32ToCpu (*P) || (Fdt32ToCpu (*P) > 500000)) {
    return EFI_COMPROMISED_DATA;
  }

  Port->DeassertUs    = Fdt32ToCpu (*P);
  Port->Aldps         = FdtGetProp (Fdt, Phy, "realtek,aldps-enable", &Length) != NULL;
  Port->DisableClkout = FdtGetProp (Fdt, Phy, "realtek,clkout-disable", &Length) != NULL;
  Status              = Delay (Fdt, Node, "tx-internal-delay-ps", &Port->TxDelay);
  if (!EFI_ERROR (Status)) {
    Status = Delay (Fdt, Node, "rx-internal-delay-ps", &Port->RxDelay);
  }

  if (!EFI_ERROR (Status)) {
    Status = K3ParseGpio (Fdt, Phy, "reset-gpios", &Port->Reset);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Pll = FdtNodeOffsetByCompatible (Fdt, -1, "spacemit,k3-pll");
  if (!K3FdtEnabled (Fdt, Pll)) {
    return EFI_NOT_FOUND;
  }

  Status = K3FdtRegister (Fdt, Pll, 0, 0x124, &Port->Pll, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  P = FdtGetProp (Fdt, Pll, "spacemit,mpmu", &Length);
  if (!P || (Length != 4)) {
    return EFI_COMPROMISED_DATA;
  }

  Mpmu = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*P));
  if (!K3FdtEnabled (Fdt, Mpmu) ||
      (FdtStringListSearch (Fdt, Mpmu, "compatible", "spacemit,k3-syscon-mpmu") < 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  Status = K3FdtRegister (Fdt, Mpmu, 0, 0x14, &Port->Mpmu, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Port->Base    = Base;
  Port->Apmu    = Apmu;
  Port->Control = Apmu + Offsets[Id];
  Port->Dline   = Port->Control + 4;
  Status        = K3MapMmio (Base, Bytes);
  if (!EFI_ERROR (Status)) {
    Status = K3MapMmio (Apmu, 0x3f4);
  }

  if (!EFI_ERROR (Status)) {
    Status = K3MapMmio (Port->Pll, 0x124);
  }

  if (!EFI_ERROR (Status)) {
    Status = K3MapMmio (Port->Mpmu, 0x14);
  }

  return Status;
}

EFI_STATUS
K3EqosPrepare (
  K3_EQOS_PORT  *P
  )
{
  if (!P || P->Owned || P->Faulted) {
    return EFI_INVALID_PARAMETER;
  }

  UINT32  Control = MmioRead32 (P->Control);

  if (((Control & 3) == 3) && ((MmioRead32 (P->Base) & 3) ||
                               (MmioRead32 (P->Base + 0x1104) & BIT0) || (MmioRead32 (P->Base + 0x1108) & BIT0)))
  {
    return EFI_ACCESS_DENIED;
  }

  // AXI and TX reference sources must be real enabled fixed-rate parents.
  UINT32  Axi = MmioRead32 (P->Apmu + 0x388), Pll1 = MmioRead32 (P->Pll + 0x104);

  if ((Axi & BIT4) || (MmioRead32 (P->Pll + 0x100) != 0x0b330ccc) ||
      (MmioRead32 (P->Pll + 0x108) != 0xa0558989) || ((Pll1 & 0xff00) != 0xcd00) ||
      !(Pll1 & BIT16) || !(Pll1 & ((Axi & BIT0) ? BIT5 : BIT7)) ||
      !(MmioRead32 (P->Mpmu + 0x10) & BIT24))
  {
    return EFI_NOT_READY;
  }

  P->CsrRate   = ((Axi & BIT0) ? 409600000U : 307200000U) / (((Axi >> 1) & 3) + 1);
  P->MdioRange = P->CsrRate < 100000000 ? 0 : P->CsrRate < 150000000 ? 1 :
                 P->CsrRate < 250000000 ? 4 : P->CsrRate <= 300000000 ? 5 : 6;
  UINT32  Pll2 = MmioRead32 (P->Pll + 0x11c);

  if ((MmioRead32 (P->Pll + 0x118) != 0x0b3e2000) || (MmioRead32 (P->Pll + 0x120) != 0xa0558c8c) ||
      (Pll2 & 0xff00) || !(Pll2 & BIT16) || !(MmioRead32 (P->Mpmu + 0x10) & BIT25))
  {
    return EFI_NOT_READY;
  }

  EFI_STATUS  Status = K3ApplyPins (P->Fdt, P->Node);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  P->ParentAdded = !(Pll2 & BIT2);
  MmioOr32 (P->Pll + 0x11c, BIT2);
  P->Owned = TRUE;
  MmioAndThenOr32 (P->Control, ~(BIT1 | BIT3 | BIT4 | BIT9 | BIT12), BIT0 | BIT8 | BIT3);
  MmioAndThenOr32 (P->Dline, ~0xff31ff31U, BIT0 | BIT16 | (P->TxDelay << 24) | (P->RxDelay << 8));
  MmioOr32 (P->Control, BIT1);
  if ((MmioRead32 (P->Control) & (BIT0 | BIT1 | BIT8 | BIT3 | BIT4)) != (BIT0 | BIT1 | BIT8 | BIT3)) {
    return EFI_DEVICE_ERROR;
  }

  UINT32  Width = (MmioRead32 (P->Base + 0x120) >> 14) & 3;

  if ((MmioRead32 (P->Base + 0x110) & 0xff) != 0x54) {
    return EFI_UNSUPPORTED;
  }

  if ((Width != 1) && (Width != 2)) {
    return EFI_UNSUPPORTED;
  }

  P->AddressBits = Width == 1 ? 40 : 48;
  Status         = K3GpioOutput (&P->Reset, TRUE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MicroSecondDelay (P->AssertUs);
  Status = K3GpioOutput (&P->Reset, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MicroSecondDelay (P->DeassertUs);
  P->Ready = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
K3EqosPark (
  K3_EQOS_PORT  *P
  )
{
  if (!P || !P->Owned) {
    return EFI_INVALID_PARAMETER;
  }

  // Caller must first stop/confirm its DMA engine before parking resources.
  MmioAnd32 (P->Control, ~BIT1);
  if (MmioRead32 (P->Control) & BIT1) {
    P->Faulted = TRUE;
    return EFI_DEVICE_ERROR;
  }

  MmioAnd32 (P->Control, ~(BIT0 | BIT8));
  if (P->ParentAdded) {
    MmioAnd32 (P->Pll + 0x11c, ~BIT2);
  }

  P->Owned = P->Ready = P->PhyConfigured = FALSE;
  return EFI_SUCCESS;
}
