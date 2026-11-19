/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>

typedef enum {
  Sg2042CpuClock,
  Sg2042SdClock,
  Sg2042EmmcClock
} SG2042_CLOCK;

// Return the configured clock rate in Hz, or zero if it cannot be determined.
UINT64
Sg2042GetClockRate (
  IN CONST VOID    *Fdt,
  IN SG2042_CLOCK  Clock
  );
