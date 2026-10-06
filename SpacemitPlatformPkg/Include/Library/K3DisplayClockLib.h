/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_DISPLAY_CLOCK_LIB_H
#define K3_DISPLAY_CLOCK_LIB_H
#include <Uefi.h>
#include <Library/K3DpuLib.h>
#include <Protocol/Clock.h>
#include <Library/K3PowerDomainLib.h>
typedef struct {
  UINT64    RequestedHz;
  UINT64    ActualHz;
  UINT32    Selector;
  UINT32    Divider;
} K3_DISPLAY_CLOCK_PLAN;
typedef struct {
  K3_DPU_RESOURCES           Resources;
  SPACEMIT_CLOCK_PROTOCOL    *Clock;
  UINT32                     Clocks[6];
  UINT32                     Resets[6];
  UINT32                     PowerDomain;
  K3_POWER_DOMAIN            Power;
  BOOLEAN                    Parsed;
  BOOLEAN                    Owned;
  BOOLEAN                    Ready;
  BOOLEAN                    Faulted;
  BOOLEAN                    DmaStarted;
  BOOLEAN                    PowerAdded;
  K3_DISPLAY_CLOCK_PLAN      Pixel;
} K3_DISPLAY_CLOCK;
EFI_STATUS
K3DisplayClockParse (
  CONST K3_DPU_RESOURCES  *Resources,
  K3_DISPLAY_CLOCK        *Clock
  );

/* Nearest validated DPU APMU parent; gates are acquired during Prepare.
   This engine clock does not replace the DP pixel PLL stream timing. */
EFI_STATUS
K3DisplayChooseClock (
  K3_DISPLAY_CLOCK       *Clock,
  UINT64                 RequestedHz,
  K3_DISPLAY_CLOCK_PLAN  *Plan
  );

EFI_STATUS
K3DisplayPrepare (
  K3_DISPLAY_CLOCK             *Clock,
  CONST K3_DISPLAY_CLOCK_PLAN  *Plan
  );

EFI_STATUS
K3DisplayGetPixelClock (
  VOID    *Clock,
  UINT64  *Hertz
  );

/* Mark before any scanout/DMA enable. ColdAbort is then permanently denied;
   live reset/bus-drain and shutdown need a separate verified native owner. */
EFI_STATUS
K3DisplayMarkDma (
  K3_DISPLAY_CLOCK  *Clock
  );

EFI_STATUS
K3DisplayColdAbort (
  K3_DISPLAY_CLOCK  *Clock
  );

#endif
