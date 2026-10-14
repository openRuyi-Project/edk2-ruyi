/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Register/RiscV64/RiscVImpl.h>
#include <Library/PcdLib.h>
#include "Cpu.h"

ASM_FUNC (_ModuleEntryPoint)
  /* Preserve the standard SBI a0 boot hart and a1 FDT arguments. */
  li t0, FixedPcdGet64 (PcdSecMemoryBase)
  li t1, FixedPcdGet64 (PcdSecMemorySize)
  add sp, t0, t1
  tail SecEntry

/* HSM enters with a0=hart and a1=opaque. Do not share hart 0's live SEC stack. */
ASM_FUNC (SecA100Entry)
  mv s0, a0
  mv s1, a1
  rdtime s2
  li s3, 24000000 /* One second at K3's 24 MHz timebase. */
1:
  li a0, 0
  li a6, K3_SBI_HART_STATUS
  li a7, K3_SBI_EXT_HSM
  ecall
  bnez a0, 3f
  li t0, K3_SBI_HART_STOPPED
  beq a1, t0, 2f
  rdtime t0
  sub t0, t0, s2
  bltu t0, s3, 1b
3:
  li a6, K3_SBI_HART_STOP
  li a7, K3_SBI_EXT_HSM
  ecall
4:
  wfi
  j 4b
2:
  fence rw, rw
  fence.i
  mv a0, s0
  mv a1, s1
  j _ModuleEntryPoint
