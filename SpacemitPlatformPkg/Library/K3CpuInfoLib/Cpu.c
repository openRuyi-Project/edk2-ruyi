/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/PlatformCpuInfoLib.h>
#include <Guid/RiscVSecHobData.h>
#include <Protocol/Clock.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define EMIT(Label, Value)  do {\
  Status = Emit (Context, (Label), (Value)); \
  if (EFI_ERROR (Status)) { return Status; } \
} while (0)

STATIC BOOLEAN
NodeEnabled (
  CONST VOID  *Fdt,
  INT32       Node
  )
{
  INT32        Length;
  CONST CHAR8  *Value = FdtGetProp (Fdt, Node, "status", &Length);

  return !Value || ((Length == 5) && !CompareMem (Value, "okay", 5)) ||
         ((Length == 3) && !CompareMem (Value, "ok", 3));
}

EFI_STATUS
PlatformCpuInfo (
  INFO_EMIT  Emit,
  VOID       *Context
  )
{
  CONST EFI_GUID           Guid = RISCV_SEC_HANDOFF_HOB_GUID;
  EFI_HOB_GUID_TYPE        *Hob = GetFirstGuidHob (&Guid);
  RISCV_SEC_HANDOFF_DATA   Handoff;
  SPACEMIT_CLOCK_PROTOCOL  *Clock;
  CONST CHAR8              *Value;
  CHAR16                   Text[128], Label[48];
  UINT32                   X100 = 0, A100 = 0, Other = 0;
  UINT64                   Rate;
  INT32                    Cpus, Node, Length;
  EFI_STATUS               Status;

  UnicodeSPrint (Text, sizeof (Text), L"%a", (CHAR8 *)FixedPcdGetPtr (PcdSystemProductName));
  EMIT (L"System", Text);
  EMIT (L"Processor", L"SpacemiT K3");
  if (!Hob || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (Handoff))) {
    return EFI_NOT_FOUND;
  }

  CopyMem (&Handoff, GET_GUID_HOB_DATA (Hob), sizeof (Handoff));
  if (!Handoff.FdtPointer || FdtCheckHeader (Handoff.FdtPointer)) {
    return EFI_COMPROMISED_DATA;
  }

  Cpus = FdtPathOffset (Handoff.FdtPointer, "/cpus");
  if (Cpus < 0) {
    return EFI_NOT_FOUND;
  }

  // Report this boot's active CPUs, not the setting saved for the next boot.
  FdtForEachSubnode (Node, Handoff.FdtPointer, Cpus) {
    Value = FdtGetProp (Handoff.FdtPointer, Node, "device_type", &Length);
    if (!Value || (Length != 4) || CompareMem (Value, "cpu", 4) ||
        !NodeEnabled (Handoff.FdtPointer, Node))
    {
      continue;
    }

    if (FdtStringListSearch (Handoff.FdtPointer, Node, "compatible", "spacemit,x100") >= 0) {
      X100++;
    } else if (FdtStringListSearch (Handoff.FdtPointer, Node, "compatible", "spacemit,a100") >= 0) {
      A100++;
    } else {
      Other++;
    }
  }

  UnicodeSPrint (Text, sizeof (Text), L"%u X100 + %u A100", X100, A100);
  EMIT (L"Enabled cores", Text);
  if (Other) {
    UnicodeSPrint (Text, sizeof (Text), L"%u", Other);
    EMIT (L"Other enabled cores", Text);
  }

  UnicodeSPrint (Text, sizeof (Text), L"%Lu", Handoff.BootHartId);
  EMIT (L"Boot hart", Text);
  Clock = NULL;
  gBS->LocateProtocol (&gSpacemitClockProtocolGuid, NULL, (VOID **)&Clock);
  if (X100) {
    for (UINT32 Cluster = 0; Cluster < 2; Cluster++) {
      UnicodeSPrint (Label, sizeof (Label), L"X100 cluster %u clock", Cluster);
      if (Clock && (Clock->Revision == 1) && Clock->GetRate &&
          !EFI_ERROR (Clock->GetRate (2 + Cluster, &Rate)) && Rate)
      {
        UnicodeSPrint (Text, sizeof (Text), L"%Lu MHz", Rate / 1000000);
        EMIT (Label, Text);
      } else {
        EMIT (Label, L"Unknown");
      }
    }
  }

  if (A100) {
    EMIT (L"A100 clock", L"Unknown");
  }

  return EFI_SUCCESS;
}
