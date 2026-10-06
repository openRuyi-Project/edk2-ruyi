/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Register/RiscV64/RiscVImpl.h>
#include <Library/PcdLib.h>

ASM_FUNC (_ModuleEntryPoint)
  /* Preserve the standard SBI a0 boot hart and a1 FDT arguments. */
  li t0, FixedPcdGet64 (PcdSecMemoryBase)
  li t1, FixedPcdGet64 (PcdSecMemorySize)
  add sp, t0, t1
  tail SecEntry
