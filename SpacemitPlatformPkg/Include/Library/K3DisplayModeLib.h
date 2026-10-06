/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_DISPLAY_MODE_LIB_H
#define K3_DISPLAY_MODE_LIB_H
#include <Uefi.h>
#include <Library/K3InnoDpLib.h>
#define K3_DISPLAY_MAX_MODES  64

/* Stream timing belongs to the DP pixel PLL. DPU APMU mux/divider
   selection is a separate engine configuration, not an EDID timing. */
typedef struct {
  UINT64    RequestedHz;
  UINT64    ActualHz;
} K3_PIXEL_CLOCK_PLAN;
typedef struct {
  SOC_DP_VIDEO_MODE      Timing;
  K3_PIXEL_CLOCK_PLAN    PixelClock;
  UINT32                 AdvertisedClockKhz;
  BOOLEAN                Preferred, Adjusted;
} K3_DISPLAY_MODE;
typedef struct {
  UINT32             Count, Rejected;
  BOOLEAN            UnhandledTiming;
  K3_DISPLAY_MODE    Modes[K3_DISPLAY_MAX_MODES];
} K3_DISPLAY_MODE_SET;
typedef EFI_STATUS (*K3_MODE_CLOCK_SELECT) (
  VOID                 *Priv,
  UINT64               Hertz,
  K3_PIXEL_CLOCK_PLAN  *Plan
  );

/* Validate the complete EDID before read-only clock callbacks. No synthetic
   fallback. Output is unchanged on malformed data, backend errors or overflow.
   LinkRateKhz is serial bit clock, with supported lane counts 1/2/4. */
EFI_STATUS
K3DisplayModes (
  CONST UINT8           *Edid,
  UINTN                 Bytes,
  UINT32                LinkRateKhz,
  UINT32                Lanes,
  K3_MODE_CLOCK_SELECT  Select,
  VOID                  *Priv,
  K3_DISPLAY_MODE_SET   *Modes
  );

#endif
