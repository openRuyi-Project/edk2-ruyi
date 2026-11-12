/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>
typedef enum {
  Eic7700GpioCardDetect,
  Eic7700GpioPhyReset
} EIC7700_GPIO_USE;
typedef struct {
  UINT32 Offset;
  UINT32 Function;
} EIC7700_GPIO_PAD;
// Validate board wiring against the selected DT group before any pad write.
EFI_STATUS
Eic7700BoardGpioPad (
  CONST VOID *Fdt, INT32 Group, EIC7700_GPIO_USE Use,
  UINT32 Bank, UINT32 Pin, EIC7700_GPIO_PAD *Pad
  );
