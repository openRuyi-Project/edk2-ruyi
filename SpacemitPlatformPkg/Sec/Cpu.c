/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiPei.h>
#include <Library/BaseLib.h>
#include <Library/BaseRiscVSbiLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/PrintLib.h>
#include "Cpu.h"

VOID EFIAPI
SecA100Entry (
  UINTN       Hart,
  CONST VOID  *Fdt
  );

STATIC EFI_STATUS
NextBootHart (
  CONST VOID  *Fdt,
  UINTN       *Hart
  )
{
  CONST UINT32  *Value;
  INT32         Chosen, Length;

  *Hart  = 0;
  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    return EFI_SUCCESS;
  }

  Value = FdtGetProp (Fdt, Chosen, "boot-hartid", &Length);
  if (Value == NULL) {
    return EFI_SUCCESS;
  }

  if (Length != sizeof (*Value)) {
    return EFI_COMPROMISED_DATA;
  }

  *Hart = Fdt32ToCpu (*Value);
  return (*Hart == 0 || *Hart == 8) ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

EFI_STATUS
SelectBootHart (
  UINTN       BootHart,
  CONST VOID  *Fdt
  )
{
  UINTN       Next;
  EFI_STATUS  Status;
  SBI_RET     Ret;

  Status = NextBootHart (Fdt, &Next);
  if (EFI_ERROR (Status) || (Next == BootHart)) {
    return Status;
  }

  if ((BootHart != 0) || (Next != 8)) {
    return EFI_UNSUPPORTED;
  }

  DEBUG ((DEBUG_INFO, "K3: transferring UEFI from hart 0 to A100 hart 8\n"));
  Ret = SbiCall (
          K3_SBI_EXT_HSM,
          K3_SBI_HART_START,
          3,
          Next,
          (UINTN)SecA100Entry,
          (UINTN)Fdt
          );
  if (Ret.Error != SBI_SUCCESS) {
    return TranslateError (Ret.Error);
  }

  // Hart 8 waits without a stack until hart 0 has stopped using the SEC stack.
  Ret = SbiCall (K3_SBI_EXT_HSM, K3_SBI_HART_STOP, 0);
  DEBUG ((DEBUG_ERROR, "K3: stopping X100 boot hart returned %ld\n", Ret.Error));
  return EFI_DEVICE_ERROR;
}

EFI_STATUS
FixupCpuStatus (
  UINTN  BootHart,
  VOID   *Fdt
  )
{
  UINTN       Next;
  EFI_STATUS  Status;
  INT32       Node;
  CHAR8       Path[32];

  Status = NextBootHart (Fdt, &Next);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (BootHart != Next) {
    return EFI_COMPROMISED_DATA;
  }

  if (Next != 8) {
    return EFI_SUCCESS;
  }

  // Preserve IRQ slot numbering: disabled X100 nodes still occupy IMSIC slots.
  // Coreboot already builds an A100-only cpu-map for this boot mode.
  for (UINTN Hart = 0; Hart < 8; Hart++) {
    AsciiSPrint (Path, sizeof (Path), "/cpus/cpu@%x", Hart);
    Node = FdtPathOffset (Fdt, Path);
    if ((Node < 0) || (FdtSetPropString (Fdt, Node, "status", "disabled") < 0)) {
      return EFI_COMPROMISED_DATA;
    }
  }

  return EFI_SUCCESS;
}
