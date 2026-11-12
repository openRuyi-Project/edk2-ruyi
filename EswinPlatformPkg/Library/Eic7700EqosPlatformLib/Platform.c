/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IndustryStandard/Eic7700ClockIds.h>
#include <IndustryStandard/Eic7700ResetIds.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/EqosLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>

#define SYS_HSP_AXI       0x148
#define SYS_HSP_CFG       0x14c
#define SYS_ETH0          0x158
#define SYS_ETH1          0x15c
#define SYS_CONFIG_CLOCK  0x20c
#define SYS_HSP_RESET     0x11c
#define ETH_GATE          BIT0
#define ETH_DIV_MASK      (0x7fU << 4)
#define ETH_ROOT_OSC      BIT1
#define ETH_PHY_RGMII     (BIT16 | BIT0)
#define ETH_RX_INVERT     BIT31
#define GPIO_PORT_STRIDE  0x0c
#define GPIO_DDR          4
#define PIN_FUNCTION      (7U << 16)
#define PIN_INPUT         BIT0
#define PIN_PULLS         (BIT1 | BIT2)
#define MAC_VERSION       0x110
#define MAC_FEATURE1      0x120
#define MAC_DMA_RESET     0x1000

typedef struct {
  EQOS_PORT Common;
  UINTN Clock, Reset, Hsp, Gpio, PinBase;
  UINT32 ClockOffset, ResetMask, HspOffset, Delay, IoVoltageOffset, IoVoltage;
  UINT32 GpioBank, GpioBit, PinIndex, CurrentSpeed;
  BOOLEAN InvertRx, ActiveLow;
} EIC_EQOS_PORT;

STATIC EFI_STATUS
Cell (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name, UINT32 *Value)
{
  INT32 Len;
  CONST UINT32 *Data = FdtGetProp (Fdt, Node, Name, &Len);
  if (!Data || Len != 4) { return EFI_COMPROMISED_DATA; }
  *Value = Fdt32ToCpu (*Data);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Provider (CONST VOID *Fdt, UINT32 Phandle, CONST CHAR8 *Type, UINTN Size, UINTN *Base)
{
  INT32 Node = FdtNodeOffsetByPhandle (Fdt, Phandle);
  if (!FdtNodeEnabled (Fdt, Node) || FdtStringListSearch (Fdt, Node, "compatible", Type) < 0) {
    return EFI_UNSUPPORTED;
  }
  return FdtGetMmio (Fdt, Node, 0, Size, Base, NULL);
}

INT32
EqosNextNode (CONST VOID *Fdt, INT32 Previous)
{
  return FdtNodeOffsetByCompatible (Fdt, Previous, "snps,dwmac-5.20");
}

STATIC EFI_STATUS
Parse (CONST VOID *Fdt, INT32 Node, EIC_EQOS_PORT *Hw)
{
  STATIC CONST UINT32 TxClockIds[] = {EIC7700_CLK_GATE_HSP_ETH0_CORE_CLK, EIC7700_CLK_GATE_HSP_ETH1_CORE_CLK};
  STATIC CONST UINT32 ResetIds[] = {EIC7700_RESET_HSP_ETH0_ARST, EIC7700_RESET_HSP_ETH1_ARST};
  EQOS_PORT *P = &Hw->Common;
  CONST UINT32 *Data;
  CONST CHAR8 *Mode, *Pin;
  INT32 Len, Gpio, Group, Pinctrl, Phy;
  UINTN Id, I;
  UINT32 Value, Rate, Tx = 0, Rx = 0;
  EFI_STATUS Status;

  Hw->InvertRx = FdtStringListSearch (Fdt, Node, "compatible", "eswin,eic7700-qos-eth-clk-inversion") >= 0;
  if (!Hw->InvertRx && FdtStringListSearch (Fdt, Node, "compatible", "eswin,eic7700-qos-eth") < 0) {
    return EFI_UNSUPPORTED;
  }
  P->Fdt = Fdt;
  P->Node = Node;
  Status = FdtGetMmio (Fdt, Node, 0, SIZE_64KB, &P->Base, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Data = FdtGetProp (Fdt, Node, "eswin,hsp-sp-csr", &Len);
  if (!Data || Len != 24) { return EFI_COMPROMISED_DATA; }
  Hw->HspOffset = Fdt32ToCpu (Data[1]);
  if ((Hw->HspOffset != 0x100 && Hw->HspOffset != 0x200) ||
      Fdt32ToCpu (Data[2]) != Hw->HspOffset + 8 ||
      Fdt32ToCpu (Data[3]) != Hw->HspOffset + 0x18 ||
      Fdt32ToCpu (Data[4]) != Hw->HspOffset + 0x14 ||
      Fdt32ToCpu (Data[5]) != Hw->HspOffset + 0x1c) { return EFI_UNSUPPORTED; }
  Id = (Hw->HspOffset - 0x100) / 0x100;
  Status = Provider (Fdt, Fdt32ToCpu (*Data), "eswin,eic7700-syscfg", Hw->HspOffset + 0x20, &Hw->Hsp);
  if (EFI_ERROR (Status)) { return Status; }
  Data = FdtGetProp (Fdt, Node, "clocks", &Len);
  if (!Data || Len != 32 || Data[0] != Data[2] || Data[0] != Data[4] || Data[0] != Data[6] ||
      Fdt32ToCpu (Data[1]) != EIC7700_CLK_GATE_HSP_ACLK || Fdt32ToCpu (Data[3]) != EIC7700_CLK_GATE_HSP_CFG_CLK ||
      Fdt32ToCpu (Data[5]) != EIC7700_CLK_MUX_SYSCFG_CLK_ROOT_2MUX1_GFREE || Fdt32ToCpu (Data[7]) != TxClockIds[Id] ||
      FdtStringListSearch (Fdt, Node, "clock-names", "axi") != 0 ||
      FdtStringListSearch (Fdt, Node, "clock-names", "cfg") != 1 ||
      FdtStringListSearch (Fdt, Node, "clock-names", "stmmaceth") != 2 ||
      FdtStringListSearch (Fdt, Node, "clock-names", "tx") != 3) { return EFI_UNSUPPORTED; }
  Status = Provider (Fdt, Fdt32ToCpu (*Data), "eswin,eic7700-clock", SYS_CONFIG_CLOCK + 4, &Hw->Clock);
  if (EFI_ERROR (Status)) { return Status; }
  Hw->ClockOffset = SYS_ETH0 + 4 * Id;
  Hw->ResetMask = BIT26 >> Id;
  Data = FdtGetProp (Fdt, Node, "resets", &Len);
  if (!Data || Len != 8 || Fdt32ToCpu (Data[1]) != ResetIds[Id]) { return EFI_UNSUPPORTED; }
  Status = Provider (Fdt, Fdt32ToCpu (*Data), "eswin,eic7700-reset", SYS_HSP_RESET + 4, &Hw->Reset);
  if (EFI_ERROR (Status)) { return Status; }
  Mode = FdtGetProp (Fdt, Node, "phy-mode", &Len);
  if (!Mode || Len < 6 || Len > 11 || Mode[Len - 1]) { return EFI_COMPROMISED_DATA; }
  if (!AsciiStrCmp (Mode, "rgmii-id")) { P->DelayFlags = 3; }
  else if (!AsciiStrCmp (Mode, "rgmii-rxid")) { P->DelayFlags = 2; }
  else if (!AsciiStrCmp (Mode, "rgmii-txid")) { P->DelayFlags = 1; }
  else if (AsciiStrCmp (Mode, "rgmii")) { return EFI_UNSUPPORTED; }
  Status = Cell (Fdt, Node, "snps,axi-config", &Value);
  if (EFI_ERROR (Status)) { return Status; }
  Group = FdtNodeOffsetByPhandle (Fdt, Value);
  Status = Cell (Fdt, Group, "snps,rd_osr_lmt", &Value);
  if (EFI_ERROR (Status) || Value > 15) { return EFI_UNSUPPORTED; }
  P->DmaBusMode = (Value << 16) | BIT11; // Extended DMA addressing.
  Status = Cell (Fdt, Group, "snps,wr_osr_lmt", &Value);
  if (EFI_ERROR (Status) || Value > 15) { return EFI_UNSUPPORTED; }
  P->DmaBusMode |= Value << 24;
  Data = FdtGetProp (Fdt, Group, "snps,blen", &Len);
  if (!Data || Len <= 0 || Len % 4) { return EFI_COMPROMISED_DATA; }
  for (I = 0; I < (UINTN)Len / 4; I++) {
    Value = Fdt32ToCpu (Data[I]);
    if (!Value) { continue; }
    if (Value < 4 || Value > 256 || (Value & (Value - 1))) { return EFI_UNSUPPORTED; }
    P->DmaBusMode |= BIT0 << (HighBitSet32 (Value) - 1);
  }
  if (FdtGetProp (Fdt, Node, "snps,aal", &Len)) { P->DmaBusMode |= BIT12; }
  if (FdtGetProp (Fdt, Node, "snps,fixed-burst", &Len)) { P->DmaBusMode |= BIT0; }
  // ETH1 has 2 ns inherent TX skew. Its PHY must not add another TX delay.
  if (Hw->InvertRx) { P->DelayFlags &= ~1U; }
  if (FdtGetProp (Fdt, Node, "tx-internal-delay-ps", &Len)) {
    Status = Cell (Fdt, Node, "tx-internal-delay-ps", &Tx);
    if (EFI_ERROR (Status) || Tx < (Hw->InvertRx ? 2000U : 0)) { return EFI_INVALID_PARAMETER; }
    Tx -= Hw->InvertRx ? 2000 : 0;
  }
  if (FdtGetProp (Fdt, Node, "rx-internal-delay-ps", &Len)) {
    Status = Cell (Fdt, Node, "rx-internal-delay-ps", &Rx);
    if (EFI_ERROR (Status)) { return Status; }
  }
  if (Tx > 2540 || Rx > 2540 || Tx % 20 || Rx % 20) { return EFI_UNSUPPORTED; }
  Hw->Delay = (Tx / 20) << 8 | (Rx / 20) << 24;
  Status = Cell (Fdt, Node, "phy-handle", &Value);
  if (EFI_ERROR (Status)) { return Status; }
  Phy = FdtNodeOffsetByPhandle (Fdt, Value);
  if (!FdtNodeEnabled (Fdt, Phy) || FdtParentOffset (Fdt, FdtParentOffset (Fdt, Phy)) != Node ||
      FdtStringListSearch (Fdt, Phy, "compatible", "ethernet-phy-id001c.c916") < 0) { return EFI_UNSUPPORTED; }
  P->PhyNode = Phy;
  Status = Cell (Fdt, Phy, "reg", &P->PhyAddress);
  if (EFI_ERROR (Status) || P->PhyAddress > 31) { return EFI_UNSUPPORTED; }
  Status = Cell (Fdt, Phy, "reset-assert-us", &P->AssertUs);
  if (EFI_ERROR (Status) || !P->AssertUs || P->AssertUs > 100000) { return EFI_UNSUPPORTED; }
  Status = Cell (Fdt, Phy, "reset-deassert-us", &P->DeassertUs);
  if (EFI_ERROR (Status) || !P->DeassertUs || P->DeassertUs > 500000) { return EFI_UNSUPPORTED; }
  P->Aldps = FdtGetProp (Fdt, Phy, "realtek,aldps-enable", &Len) != NULL;
  P->DisableClkout = FdtGetProp (Fdt, Phy, "realtek,clkout-disable", &Len) != NULL;
  Data = FdtGetProp (Fdt, Phy, "reset-gpios", &Len);
  if (!Data || Len != 12 || Fdt32ToCpu (Data[2]) > 1) { return EFI_UNSUPPORTED; }
  Gpio = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Data));
  Hw->GpioBit = Fdt32ToCpu (Data[1]);
  Hw->ActiveLow = Fdt32ToCpu (Data[2]) != 0;
  Status = Cell (Fdt, Gpio, "reg", &Hw->GpioBank);
  if (EFI_ERROR (Status) || Hw->GpioBank != 3 || (Hw->GpioBit != 10 && Hw->GpioBit != 15)) { return EFI_UNSUPPORTED; }
  Status = FdtGetMmio (Fdt, FdtParentOffset (Fdt, Gpio), 0, 0x80, &Hw->Gpio, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Cell (Fdt, Node, "pinctrl-0", &Value);
  if (EFI_ERROR (Status)) { return Status; }
  Group = FdtNodeOffsetByPhandle (Fdt, Value);
  Pin = Hw->GpioBit == 10 ? "gpio106" : "gpio111";
  if (FdtStringListCount (Fdt, Group, "pins") != 1 || FdtStringListSearch (Fdt, Group, "pins", Pin) != 0 ||
      FdtStringListSearch (Fdt, Group, "function", "gpio") != 0) { return EFI_UNSUPPORTED; }
  Hw->PinIndex = Hw->GpioBit == 10 ? 153 : 158;
  Pinctrl = Group;
  do { Pinctrl = FdtParentOffset (Fdt, Pinctrl); }
  while (Pinctrl > 0 && FdtStringListSearch (Fdt, Pinctrl, "compatible", "eswin,eic7700-pinctrl") < 0);
  if (Pinctrl <= 0) { return EFI_NOT_FOUND; }
  Status = FdtGetMmio (Fdt, Pinctrl, 0, 4 * (Hw->PinIndex + 1), &Hw->PinBase, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Cell (Fdt, Pinctrl, "vrgmii-supply", &Value);
  if (EFI_ERROR (Status)) { return Status; }
  Group = FdtNodeOffsetByPhandle (Fdt, Value);
  Status = Cell (Fdt, Group, "regulator-min-microvolt", &Value);
  if (EFI_ERROR (Status) || (Value != 1800000 && Value != 3300000)) { return EFI_UNSUPPORTED; }
  Hw->IoVoltage = Value == 1800000 ? 3 : 0;
  Status = Cell (Fdt, Group, "regulator-max-microvolt", &Rate);
  if (EFI_ERROR (Status) || Rate != Value) { return EFI_UNSUPPORTED; }
  Hw->IoVoltageOffset = 0x290 + Id * 4;
  Status = FdtMapMmio (P->Base, SIZE_64KB);
  if (!EFI_ERROR (Status)) { Status = FdtMapMmio (Hw->Clock, SYS_CONFIG_CLOCK + 4); }
  if (!EFI_ERROR (Status)) { Status = FdtMapMmio (Hw->Reset, SYS_HSP_RESET + 4); }
  if (!EFI_ERROR (Status)) { Status = FdtMapMmio (Hw->Hsp, Hw->HspOffset + 0x20); }
  if (!EFI_ERROR (Status)) { Status = FdtMapMmio (Hw->Gpio, 0x80); }
  if (!EFI_ERROR (Status)) { Status = FdtMapMmio (Hw->PinBase, Hw->IoVoltageOffset + 4); }
  if (EFI_ERROR (Status)) { return Status; }
  // The CSR clock is the SYSCRG configuration root, not the TX clock.
  Value = MmioRead32 (Hw->Clock + SYS_CONFIG_CLOCK);
  if (Value & BIT0) { Rate = 24000000; }
  else { Rate = 400000000U / MAX (2U, (Value >> 4) & 7); }
  P->CsrRate = Rate;
  P->MdioRange = Rate < 100000000 ? 0 : Rate < 150000000 ? 1 : Rate < 250000000 ? 4 : 5;
  return EFI_SUCCESS;
}

EFI_STATUS
EqosParse (CONST VOID *Fdt, INT32 Node, EQOS_PORT **Port)
{
  EIC_EQOS_PORT *Hw;
  EFI_STATUS Status;
  if (!Port) { return EFI_INVALID_PARAMETER; }
  *Port = NULL;
  Hw = AllocateZeroPool (sizeof (*Hw));
  if (!Hw) { return EFI_OUT_OF_RESOURCES; }
  Status = Parse (Fdt, Node, Hw);
  if (EFI_ERROR (Status)) { FreePool (Hw); return Status; }
  *Port = &Hw->Common;
  return EFI_SUCCESS;
}

EFI_STATUS
EqosSetLink (EQOS_PORT *P, UINT32 Speed, BOOLEAN FullDuplex)
{
  EIC_EQOS_PORT *Hw = (EIC_EQOS_PORT *)P;
  UINT32 Rate, Parent, Divider, Delay;
  if (!P || !P->Owned) { return EFI_NOT_READY; }
  if (Hw->CurrentSpeed == Speed) { return EFI_SUCCESS; }
  switch (Speed) {
    case 1000: Rate = 125000000; break;
    case 100: Rate = 25000000; break;
    case 10: Rate = 2500000; break;
    default: return EFI_UNSUPPORTED;
  }
  // The root mux is shared by ETH0 and ETH1. Do not change it per port.
  Parent = (MmioRead32 (Hw->Clock + SYS_ETH0) & ETH_ROOT_OSC) ? 24000000 : 250000000;
  if (Parent % Rate || Parent / Rate < 2 || Parent / Rate > 127) { return EFI_UNSUPPORTED; }
  Divider = Parent / Rate;
  MmioAndThenOr32 (Hw->Clock + Hw->ClockOffset, ~ETH_DIV_MASK, Divider << 4 | ETH_GATE);
  Delay = Hw->Delay | ((Hw->InvertRx && Speed == 1000) ? ETH_RX_INVERT : 0);
  MmioWrite32 (Hw->Hsp + Hw->HspOffset + 0x18, Delay);
  Hw->CurrentSpeed = Speed;
  DEBUG ((DEBUG_INFO, "EIC7700 EQoS %lx TX clock %u Hz\n", P->Base, Rate));
  return EFI_SUCCESS;
}

EFI_STATUS
EqosPrepare (EQOS_PORT *P)
{
  EIC_EQOS_PORT *Hw = (EIC_EQOS_PORT *)P;
  UINTN Reg;
  UINT32 Level, Width;
  EFI_STATUS Status;
  if (!P || P->Owned || P->Faulted) { return EFI_INVALID_PARAMETER; }
  MmioOr32 (Hw->Clock + SYS_HSP_AXI, BIT31);
  MmioOr32 (Hw->Clock + SYS_HSP_CFG, BIT31);
  // Hold this MAC in reset while establishing its required TX clock.
  MmioAnd32 (Hw->Reset + SYS_HSP_RESET, ~Hw->ResetMask);
  P->Owned = TRUE;
  Status = EqosSetLink (P, 1000, TRUE);
  if (EFI_ERROR (Status)) { return Status; }
  MmioAndThenOr32 (Hw->PinBase + Hw->IoVoltageOffset, ~3U, Hw->IoVoltage);
  MmioOr32 (Hw->Hsp + Hw->HspOffset, ETH_PHY_RGMII);
  MmioWrite32 (Hw->Hsp + Hw->HspOffset + 8, BIT0);
  MmioWrite32 (Hw->Hsp + Hw->HspOffset + 0x14, 0);
  MmioWrite32 (Hw->Hsp + Hw->HspOffset + 0x1c, 0);
  MicroSecondDelay (2);
  MmioOr32 (Hw->Reset + SYS_HSP_RESET, Hw->ResetMask);
  if ((MmioRead32 (P->Base + MAC_VERSION) & 0xff) != 0x52) { return EFI_UNSUPPORTED; }
  Width = (MmioRead32 (P->Base + MAC_FEATURE1) >> 14) & 3;
  if (Width != 1 && Width != 2) { return EFI_UNSUPPORTED; }
  P->AddressBits = Width == 1 ? 40 : 48;
  // GPIO output is latched before enabling the pad output direction.
  Reg = Hw->Gpio + Hw->GpioBank * GPIO_PORT_STRIDE;
  Level = Hw->ActiveLow ? 0 : BIT0 << Hw->GpioBit;
  MmioAndThenOr32 (Reg, ~(BIT0 << Hw->GpioBit), Level);
  MmioOr32 (Reg + GPIO_DDR, BIT0 << Hw->GpioBit);
  MmioAnd32 (Hw->PinBase + 4 * Hw->PinIndex, ~(PIN_FUNCTION | PIN_INPUT | PIN_PULLS));
  MicroSecondDelay (P->AssertUs);
  MmioAndThenOr32 (Reg, ~(BIT0 << Hw->GpioBit), Level ^ (BIT0 << Hw->GpioBit));
  MicroSecondDelay (P->DeassertUs);
  P->Ready = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
EqosPark (EQOS_PORT *P)
{
  EIC_EQOS_PORT *Hw = (EIC_EQOS_PORT *)P;
  if (!P || !P->Owned) { return EFI_INVALID_PARAMETER; }
  MmioAnd32 (Hw->Reset + SYS_HSP_RESET, ~Hw->ResetMask);
  if (MmioRead32 (Hw->Reset + SYS_HSP_RESET) & Hw->ResetMask) { P->Faulted = TRUE; return EFI_DEVICE_ERROR; }
  MmioAnd32 (Hw->Clock + Hw->ClockOffset, ~ETH_GATE);
  P->Owned = P->Ready = P->PhyConfigured = FALSE;
  Hw->CurrentSpeed = 0;
  return EFI_SUCCESS;
}
