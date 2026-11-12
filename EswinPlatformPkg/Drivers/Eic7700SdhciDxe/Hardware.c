/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IndustryStandard/Eic7700ClockIds.h>
#include <IndustryStandard/Eic7700ResetIds.h>
#include "Host.h"
#include <Library/Eic7700BoardGpioLib.h>
#include <IndustryStandard/Eic7700PinControl.h>

#define SYSCRG_HSP_AXI       0x148
#define SYSCRG_HSP_CFG       0x14c
#define SYSCRG_MSHC0         0x160
#define SYSCRG_HSP_RESET     0x11c
#define CORE_CLOCK_GATE      BIT16
#define CORE_CLOCK_DIV_MASK  (0xfffU << 4)
#define CORE_CLOCK_PARENT    BIT0
#define HSP_MMC_TIMER_GATE  BIT8
#define HSP_MMC_BUS_GATE    BIT24
#define HSP_CLOCK_STABLE     (BIT28 | BIT16 | BIT8 | BIT0)
#define GPIO_DIRECTION       0x04
#define GPIO_PORT_STRIDE     0x0c
#define GPIO_EXTERNAL        0x50

// EIC7700 reset-controller binding IDs.
STATIC EFI_STATUS
Cell (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name, UINT32 *Value)
{
  CONST UINT32 *Data;
  INT32 Length;
  Data = FdtGetProp (Fdt, Node, Name, &Length);
  if (!Data || Length != sizeof (*Data)) { return EFI_COMPROMISED_DATA; }
  *Value = Fdt32ToCpu (*Data);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Resource (CONST VOID *Fdt, UINT32 Phandle, CONST CHAR8 *Compatible,
          UINTN Minimum, UINTN *Base)
{
  INT32 Node = FdtNodeOffsetByPhandle (Fdt, Phandle);
  if (!FdtNodeEnabled (Fdt, Node) ||
      FdtStringListSearch (Fdt, Node, "compatible", Compatible) < 0)
  {
    return EFI_UNSUPPORTED;
  }
  return FdtGetMmio (Fdt, Node, 0, Minimum, Base, NULL);
}

STATIC EFI_STATUS
CardDetectPins (CONST VOID *Fdt, INT32 Node, EIC_MMC_PORT *Port)
{
  CONST UINT32 *Data;
  INT32 Length, Gpio, Parent, Group, PinController;
  UINT32 Bank, Pin, Flags;
  UINTN PinBase;
  EIC7700_GPIO_PAD Pad;
  EFI_STATUS Status;

  Data = FdtGetProp (Fdt, Node, "cd-gpios", &Length);
  if (!Data || Length != 12) { return EFI_COMPROMISED_DATA; }
  Gpio = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Data[0]));
  Pin = Fdt32ToCpu (Data[1]);
  Flags = Fdt32ToCpu (Data[2]);
  if (EFI_ERROR (Cell (Fdt, Gpio, "reg", &Bank)) || !FdtNodeEnabled (Fdt, Gpio) ||
      FdtStringListSearch (Fdt, Gpio, "compatible", "snps,dw-apb-gpio-port") < 0 ||
      Bank >= EIC7700_GPIO_BANKS || Pin >= EIC7700_GPIO_PINS_PER_BANK || Flags > 1)
  {
    return EFI_UNSUPPORTED;
  }
  Parent = FdtParentOffset (Fdt, Gpio);
  Status = FdtGetMmio (Fdt, Parent, 0, GPIO_EXTERNAL + (Bank + 1) * sizeof (UINT32), &Port->GpioBase, NULL);
  if (EFI_ERROR (Status)) { return Status; }

  Data = FdtGetProp (Fdt, Node, "pinctrl-0", &Length);
  if (!Data || Length != 4) { return EFI_COMPROMISED_DATA; }
  Group = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Data));
  Status = Eic7700BoardGpioPad (Fdt, Group, Eic7700GpioCardDetect, Bank, Pin, &Pad);
  if (EFI_ERROR (Status)) { return Status; }
  PinController = FdtParentOffset (Fdt, Group);
  while (PinController > 0 &&
         FdtStringListSearch (Fdt, PinController, "compatible", "eswin,eic7700-pinctrl") < 0)
  {
    PinController = FdtParentOffset (Fdt, PinController);
  }
  if (PinController <= 0) {
    return EFI_UNSUPPORTED;
  }
  Status = FdtGetMmio (Fdt, PinController, 0, Pad.Offset + sizeof (UINT32), &PinBase, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (Port->GpioBase, 0x80);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (PinBase, Pad.Offset + sizeof (UINT32));
  if (EFI_ERROR (Status)) { return Status; }
  // The board provider has validated the DT wiring and pad selection.
  MmioAndThenOr32 (PinBase + Pad.Offset, ~EIC7700_PIN_FUNCTION_MASK, Pad.Function | EIC7700_PIN_INPUT_ENABLE);
  MmioAnd32 (Port->GpioBase + Bank * GPIO_PORT_STRIDE + GPIO_DIRECTION, ~((UINT32)BIT0 << Pin));
  Port->GpioBank = Bank;
  Port->GpioBit = Pin;
  Port->ActiveLow = Flags != 0;
  return EFI_SUCCESS;
}

EFI_STATUS
ParsePort (CONST VOID *Fdt, INT32 Node, EIC_MMC_PORT *Port)
{
  CONST UINT32 *Data;
  INT32 Length, Index;
  UINTN Provider, Id;
  UINT32 Rate, Width, Ohms;
  EFI_STATUS Status;
  STATIC CONST UINT32 ResetIds[2][4] = {
    {EIC7700_RESET_HSP_EMMC_ARST, EIC7700_RESET_MSHC0_PHY, EIC7700_RESET_HSP_EMMC_PRST, EIC7700_RESET_MSHC0_TXRX},
    {EIC7700_RESET_HSP_SD0_ARST, EIC7700_RESET_MSHC1_PHY, EIC7700_RESET_HSP_SD0_PRST, EIC7700_RESET_MSHC1_TXRX}
  };
  STATIC CONST CHAR8 *ResetNames[] = {"axi", "phy", "prstn", "txrx"};

  Status = FdtGetMmio (Fdt, Node, 0, EIC_MMC_REGISTER_SIZE, &Port->Base, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Data = FdtGetProp (Fdt, Node, "eswin,hsp-sp-csr", &Length);
  if (!Data || Length != 12) { return EFI_COMPROMISED_DATA; }
  Port->HspStable = Fdt32ToCpu (Data[1]);
  Port->HspPower = Fdt32ToCpu (Data[2]);
  if ((Port->HspStable != 0x508 && Port->HspStable != 0x608) ||
      Port->HspPower != Port->HspStable + 4)
  {
    return EFI_UNSUPPORTED;
  }
  Id = (Port->HspStable - 0x508) / 0x100;
  Status = Resource (Fdt, Fdt32ToCpu (*Data), "eswin,eic7700-syscfg", Port->HspPower + 8, &Port->Hsp);
  if (EFI_ERROR (Status)) { return Status; }
  Port->Emmc = FdtGetProp (Fdt, Node, "non-removable", &Length) != NULL;
  if (Port->Emmc != (Id == 0)) { return EFI_UNSUPPORTED; }
  Status = Cell (Fdt, Node, "bus-width", &Width);
  if (EFI_ERROR (Status) || Width != (Port->Emmc ? 8 : 4)) { return EFI_UNSUPPORTED; }
  Port->Width = Width;
  Status = Cell (Fdt, Node, "max-frequency", &Rate);
  if (EFI_ERROR (Status) || Rate < 400000 || Rate > (Port->Emmc ? 200000000U : 208000000U)) {
    return EFI_UNSUPPORTED;
  }
  Port->MaxClock = Rate;
  Port->Hs400 = Port->Emmc && FdtGetProp (Fdt, Node, "mmc-hs400-1_8v", &Length) != NULL;
  Status = Cell (Fdt, Node, "eswin,drive-impedance-ohms", &Ohms);
  if (EFI_ERROR (Status)) { return Status; }
  switch (Ohms) {
    case 100: Port->Drive = 0x00; break;
    case 66: Port->Drive = 0x44; break;
    case 50: Port->Drive = 0x88; break;
    case 40: Port->Drive = 0xcc; break;
    case 33: Port->Drive = 0xee; break;
    default: return EFI_UNSUPPORTED;
  }
  Port->Delay = Port->Emmc ? 0x17 : 0x55;

  Data = FdtGetProp (Fdt, Node, "clocks", &Length);
  if (!Data || Length != 24 || Fdt32ToCpu (Data[1]) != EIC7700_CLK_GATE_HSP_MSHC0_CORE_CLK + Id ||
      Fdt32ToCpu (Data[3]) != EIC7700_CLK_GATE_HSP_CFG_CLK || Fdt32ToCpu (Data[5]) != EIC7700_CLK_GATE_HSP_ACLK ||
      Data[0] != Data[2] || Data[0] != Data[4] ||
      FdtStringListSearch (Fdt, Node, "clock-names", "core") != 0 ||
      FdtStringListSearch (Fdt, Node, "clock-names", "bus") != 1 ||
      FdtStringListSearch (Fdt, Node, "clock-names", "axi") != 2)
  {
    return EFI_COMPROMISED_DATA;
  }
  Port->ClockControl = SYSCRG_MSHC0 + Id * 4;
  Status = Resource (Fdt, Fdt32ToCpu (*Data), "eswin,eic7700-clock", Port->ClockControl + 4, &Port->Clock);
  if (EFI_ERROR (Status)) { return Status; }
  Data = FdtGetProp (Fdt, Node, "resets", &Length);
  if (!Data || Length != 32) { return EFI_COMPROMISED_DATA; }
  for (Index = 0; Index < 4; Index++) {
    if (Fdt32ToCpu (Data[Index * 2 + 1]) != ResetIds[Id][Index] ||
        FdtStringListSearch (Fdt, Node, "reset-names", ResetNames[Index]) != Index)
    {
      return EFI_UNSUPPORTED;
    }
    Status = Resource (Fdt, Fdt32ToCpu (Data[Index * 2]), "eswin,eic7700-reset", SYSCRG_HSP_RESET + 4, &Provider);
    if (EFI_ERROR (Status)) { return Status; }
    if (Index && Provider != Port->Reset) { return EFI_UNSUPPORTED; }
    Port->Reset = Provider;
  }
  Port->ResetMask = (BIT0 << (23 - Id)) | (BIT0 << (3 + Id)) |
                    (BIT0 << (19 - Id)) | (BIT0 << (6 + Id));
  if (!Port->Emmc) {
    Status = CardDetectPins (Fdt, Node, Port);
    if (EFI_ERROR (Status)) { return Status; }
  }
  return EFI_SUCCESS;
}

EFI_STATUS
SetCardClock (EIC_MMC_PORT *Port, UINT32 Hertz)
{
  // EIC7700 exposes SPLL0/FOUT3 and SPLL2/FOUT3 as its two MMC roots.
  STATIC CONST UINT32 Roots[] = {400000000, 416000000};
  UINT32 Best = 0, Parent = 0, Divider = 0, Div, Rate, I;
  UINT16 Control;

  if (!Hertz || Hertz > Port->MaxClock) { return EFI_INVALID_PARAMETER; }
  for (I = 0; I < ARRAY_SIZE (Roots); I++) {
    Div = (Roots[I] + Hertz - 1) / Hertz;
    if (!Div || Div > 0xfff) { continue; }
    Rate = Roots[I] / Div;
    if (Rate > Best) { Best = Rate; Divider = Div; Parent = I; }
  }
  if (!Best) { return EFI_UNSUPPORTED; }
  Control = MmioRead16 (Port->Base + HC_CLOCK_CTRL);
  MmioWrite16 (Port->Base + HC_CLOCK_CTRL, Control & ~HC_CARD_CLOCK);
  MmioAnd32 (Port->Clock + Port->ClockControl, ~((UINT32)CORE_CLOCK_GATE));
  MmioAndThenOr32 (Port->Clock + Port->ClockControl,
                  ~((UINT32)(CORE_CLOCK_DIV_MASK | CORE_CLOCK_PARENT)),
                  (Divider << 4) | Parent | CORE_CLOCK_GATE);
  MmioWrite32 (Port->Hsp + Port->HspStable, HSP_CLOCK_STABLE);
  // Card frequency comes from SYSCRG. Bypass the host's standard divider.
  MmioWrite16 (Port->Base + HC_CLOCK_CTRL, Control & ~HC_CLOCK_DIV_MASK);
  DEBUG ((DEBUG_INFO, "EIC7700 %a clock: %u Hz\n", Port->Emmc ? "eMMC" : "SD", Best));
  return EFI_SUCCESS;
}

VOID
SetDelay (EIC_MMC_PORT *Port, UINT8 Delay)
{
  MmioWrite8 (Port->Base + PHY_CLOCK_DELAY_CFG, PHY_DELAY_UPDATE);
  MmioWrite8 (Port->Base + PHY_CLOCK_DELAY, Delay & 0x7f);
  MmioWrite8 (Port->Base + PHY_CLOCK_DELAY_CFG, 0);
}

EFI_STATUS
ConfigurePhy (EIC_MMC_PORT *Port)
{
  UINTN Timeout;
  UINT32 Drive = ((Port->Drive & 0xf) << 16) | ((Port->Drive >> 4) << 20);
  if (Port->Emmc) {
    MmioOr16 (Port->Base + Port->Vendor + DWC_EMMC_CTRL, DWC_CARD_IS_EMMC);
  }
  MmioAndThenOr32 (Port->Base + PHY_CONFIG, ~(PHY_DRIVE_MASK | PHY_RESET_N), Drive);
  MmioWrite16 (Port->Base + PHY_CMD_PAD, PHY_SLEW | PHY_PULL_UP | PHY_RX_1V8);
  MmioWrite16 (Port->Base + PHY_DATA_PAD, PHY_SLEW | PHY_PULL_UP | PHY_RX_1V8);
  MmioWrite16 (Port->Base + PHY_RESET_PAD, PHY_SLEW | PHY_PULL_UP | PHY_RX_1V8);
  MmioWrite16 (Port->Base + PHY_CLOCK_PAD, PHY_SLEW);
  if (Port->Emmc) {
    MmioWrite16 (Port->Base + PHY_STROBE_PAD, PHY_SLEW | PHY_RX_1V8);
  }
  MicroSecondDelay (2000);
  MmioOr32 (Port->Base + PHY_CONFIG, PHY_RESET_N);
  for (Timeout = 0; Timeout < 5000; Timeout++) {
    if (MmioRead32 (Port->Base + PHY_CONFIG) & PHY_POWER_GOOD) { break; }
    MicroSecondDelay (1);
  }
  if (Timeout == 5000) { return EFI_TIMEOUT; }
  SetDelay (Port, Port->Delay);
  MmioWrite8 (Port->Base + PHY_DLL_CONTROL, 0);
  MmioAnd16 (Port->Base + HC_HOST_CTRL2, (UINT16)~HC_PRESET_ENABLE);
  RefreshCardDetect (Port);
  return EFI_SUCCESS;
}

VOID
RefreshCardDetect (EIC_MMC_PORT *Port)
{
  BOOLEAN Present;
  EFI_TPL Old;
  if (Port->Emmc || !Port->GpioBase) { return; }
  Old = gBS->RaiseTPL (TPL_NOTIFY);
  Present = !!(MmioRead32 (Port->GpioBase + GPIO_EXTERNAL + Port->GpioBank * 4) & (BIT0 << Port->GpioBit));
  Present ^= Port->ActiveLow;
  MmioAndThenOr8 (Port->Base + HC_HOST_CTRL, (UINT8)~HC_CD_TEST_PRESENT,
                  HC_CD_TEST_SELECT | (Present ? HC_CD_TEST_PRESENT : 0));
  gBS->RestoreTPL (Old);
}

EFI_STATUS
ResetCommandData (EIC_MMC_PORT *Port)
{
  UINTN Timeout;
  MmioWrite8 (Port->Base + HC_RESET, HC_RESET_CMD_DATA);
  for (Timeout = 0; Timeout < 10000; Timeout++) {
    if (!(MmioRead8 (Port->Base + HC_RESET) & HC_RESET_CMD_DATA)) { break; }
    MicroSecondDelay (1);
  }
  if (Timeout == 10000) { return EFI_TIMEOUT; }
  MmioWrite16 (Port->Base + HC_INT_STATUS, MAX_UINT16);
  MmioWrite16 (Port->Base + HC_ERROR_STATUS, MAX_UINT16);
  return EFI_SUCCESS;
}

EFI_STATUS
SetDll (EIC_MMC_PORT *Port)
{
  UINTN Timeout;
  UINT8 Status;
  MmioAndThenOr32 (Port->Base + Port->Vendor + DWC_AT_CTRL, ~DWC_POST_DELAY_MASK, 2U << 19);
  MmioWrite8 (Port->Base + PHY_DLL_CONFIG1, (2U << 4) | 3);
  MmioWrite8 (Port->Base + PHY_DLL_CONFIG2, 2);
  MmioWrite8 (Port->Base + PHY_DLL_DELAY, 3U << 5);
  MmioWrite8 (Port->Base + PHY_DLL_OFFSET, 0);
  MmioWrite16 (Port->Base + PHY_DLL_TIMER, MAX_UINT16);
  MmioWrite8 (Port->Base + PHY_DLL_CONTROL, 1);
  for (Timeout = 0; Timeout < 1000000; Timeout += 100) {
    Status = MmioRead8 (Port->Base + PHY_DLL_STATUS);
    if (Status & PHY_DLL_LOCK) {
      return Status & PHY_DLL_ERROR ? EFI_DEVICE_ERROR : EFI_SUCCESS;
    }
    MicroSecondDelay (100);
  }
  return EFI_TIMEOUT;
}

EFI_STATUS
InitializePort (EIC_MMC_PORT *Port)
{
  EFI_STATUS Status;
  Status = FdtMapMmio (Port->Base, EIC_MMC_REGISTER_SIZE);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (Port->Clock, Port->ClockControl + 4);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (Port->Reset, SYSCRG_HSP_RESET + 4);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (Port->Hsp, Port->HspPower + 8);
  if (EFI_ERROR (Status)) { return Status; }
  MmioOr32 (Port->Clock + SYSCRG_HSP_AXI, BIT31);
  MmioOr32 (Port->Clock + SYSCRG_HSP_CFG, BIT31);
  MmioAnd32 (Port->Reset + SYSCRG_HSP_RESET, ~Port->ResetMask);
  MicroSecondDelay (2000);
  MmioOr32 (Port->Reset + SYSCRG_HSP_RESET, Port->ResetMask);
  MmioOr32 (Port->Hsp + Port->HspStable + 8, HSP_MMC_TIMER_GATE | HSP_MMC_BUS_GATE);
  MmioWrite32 (Port->Hsp + Port->HspStable, HSP_CLOCK_STABLE);
  MmioOr32 (Port->Hsp + Port->HspPower, BIT0);
  Status = SetCardClock (Port, 400000);
  if (EFI_ERROR (Status)) { return Status; }
  Port->Vendor = MmioRead16 (Port->Base + HC_VENDOR_POINTER) & 0xfff;
  if (Port->Vendor < 0x100 || Port->Vendor > 0xf000) { return EFI_DEVICE_ERROR; }
  MmioAnd32 (Port->Base + Port->Vendor + DWC_HOST_CTRL3, ~((UINT32)BIT0));
  return EFI_SUCCESS;
}
