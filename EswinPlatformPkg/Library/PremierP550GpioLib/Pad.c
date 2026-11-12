/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/Eic7700BoardGpioLib.h>
#include <Library/FdtLib.h>
#include <IndustryStandard/Eic7700PinControl.h>

// Carrier wiring: SD detect on A13, Ethernet PHY reset on D10 and D15.
STATIC CONST struct {
  EIC7700_GPIO_USE Use;
  UINT32 Bank, Pin;
  CONST CHAR8 *Name;
  EIC7700_GPIO_PAD Pad;
} mPins[] = {
  {Eic7700GpioCardDetect, 0, 13, "mode_set0", {EIC7700_PIN_MODE_SET0, EIC7700_PIN_MODE_SET0_GPIO}},
  {Eic7700GpioPhyReset, 3, 10, "gpio106", {EIC7700_PIN_GPIO106, EIC7700_PIN_GPIO_DEFAULT}},
  {Eic7700GpioPhyReset, 3, 15, "gpio111", {EIC7700_PIN_GPIO111, EIC7700_PIN_GPIO_DEFAULT}}
};

EFI_STATUS
Eic7700BoardGpioPad (CONST VOID *Fdt, INT32 Group, EIC7700_GPIO_USE Use,
                    UINT32 Bank, UINT32 Pin, EIC7700_GPIO_PAD *Pad)
{
  if (Fdt == NULL || Pad == NULL || Group < 0) { return EFI_INVALID_PARAMETER; }
  if (FdtStringListCount (Fdt, Group, "pins") != 1 ||
      FdtStringListCount (Fdt, Group, "function") != 1 ||
      FdtStringListSearch (Fdt, Group, "function", "gpio") != 0) {
    return EFI_UNSUPPORTED;
  }
  for (UINTN Index = 0; Index < ARRAY_SIZE (mPins); Index++) {
    if (Use == mPins[Index].Use && Bank == mPins[Index].Bank && Pin == mPins[Index].Pin &&
        FdtStringListSearch (Fdt, Group, "pins", mPins[Index].Name) == 0) {
      *Pad = mPins[Index].Pad;
      return EFI_SUCCESS;
    }
  }
  return EFI_UNSUPPORTED;
}
