/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_SEC_CPU_H_
#define K3_SEC_CPU_H_

#define K3_SBI_EXT_HSM       0x48534d
#define K3_SBI_HART_START    0
#define K3_SBI_HART_STOP     1
#define K3_SBI_HART_STATUS   2
#define K3_SBI_HART_STOPPED  1

#ifndef __ASSEMBLY__
EFI_STATUS
SelectBootHart (
  UINTN       BootHart,
  CONST VOID  *Fdt
  );

EFI_STATUS
FixupCpuStatus (
  UINTN  BootHart,
  VOID   *Fdt
  );

#endif
#endif
