/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_DPU_LIB_H
#define K3_DPU_LIB_H
#include <Uefi.h>
#include <Library/K3DisplaySurfaceLib.h>
#include <Library/K3InnoDpLib.h>
typedef struct {
  CONST VOID    *Fdt;
  INT32         DpuNode, DpNode;
  UINTN         Dpu, Dp, Apmu;
} K3_DPU_RESOURCES;
typedef struct {
  /* Confirm power/reset ownership and return the measured/decoded
     DPU engine clock. This is independent of the DP pixel PLL stream clock.
     Called before touching DPU registers; no nominal-rate fallback. */
  EFI_STATUS (*GetPixelClock)(
    VOID    *Priv,
    UINT64  *Hertz
    );

  /* Reset and prove the outbound DMA path drained within the budget, leaving
     accessible registers reset/disabled. A CFG/EOF interrupt or readback
     alone is insufficient. This is a mandatory platform-owner operation. */
  EFI_STATUS (*ResetAndDrain)(
    VOID   *Priv,
    UINTN  BudgetUs
    );
} K3_DPU_OWNER_OPS;
typedef struct {
  K3_DPU_RESOURCES      Resources;
  K3_DPU_OWNER_OPS      Ops;
  VOID                  *Priv;
  K3_DISPLAY_SURFACE    *Surface;
  SOC_DP_VIDEO_MODE     Mode;
  BOOLEAN               Bound, Active, Faulted;
  EFI_STATUS            CleanupError;
} K3_DPU;
/* Validate standard K3 DT resources/topology before any MMIO mutation. */
EFI_STATUS
K3DpuParse (
  CONST VOID        *Fdt,
  INT32             DpuNode,
  K3_DPU_RESOURCES  *Resources
  );

EFI_STATUS
K3DpuBind (
  K3_DPU                  *Dpu,
  CONST K3_DPU_RESOURCES  *Resources,
  CONST K3_DPU_OWNER_OPS  *Ops,
  VOID                    *Priv
  );

EFI_STATUS
K3DpuStart (
  K3_DPU                   *Dpu,
  K3_DISPLAY_SURFACE       *Surface,
  CONST SOC_DP_VIDEO_MODE  *Mode,
  UINT64                   ExpectedDpuClockHz
  );

EFI_STATUS
K3DpuStop (
  K3_DPU  *Dpu
  );

#endif
