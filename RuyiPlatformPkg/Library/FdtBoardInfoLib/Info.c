/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Guid/FdtHob.h>
#include <Guid/RiscVSecHobData.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtBoardInfoLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>

EFI_STATUS
FdtBoardInfoGet (
  RUYI_BOARD_INFO  *Info,
  CONST VOID       **DeviceTree OPTIONAL
  )
{
  CONST EFI_GUID          HandoffGuid = RISCV_SEC_HANDOFF_HOB_GUID;
  EFI_HOB_GUID_TYPE       *GuidHob;
  EFI_HOB_GENERIC_HEADER  *Hob;
  CONST VOID              *Fdt;
  CONST CHAR8             *Value;
  RISCV_SEC_HANDOFF_DATA  Handoff;
  INT32                   Node, Cpus, Length;

  if (!Info) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Info, sizeof (*Info));
  GuidHob = GetFirstGuidHob (&gFdtHobGuid);
  if (!GuidHob || (GET_GUID_HOB_DATA_SIZE (GuidHob) < sizeof (UINT64))) {
    return EFI_NOT_FOUND;
  }

  Fdt = (CONST VOID *)(UINTN)ReadUnaligned64 (GET_GUID_HOB_DATA (GuidHob));
  if (FdtCheckHeader (Fdt)) {
    return EFI_COMPROMISED_DATA;
  }

  Info->Model = FdtGetProp (Fdt, 0, "model", &Length);
  if (!Info->Model || (Length < 2) || (Length > 256) || Info->Model[Length - 1]) {
    return EFI_COMPROMISED_DATA;
  }

  GuidHob = GetFirstGuidHob (&HandoffGuid);
  if (!GuidHob || (GET_GUID_HOB_DATA_SIZE (GuidHob) < sizeof (Handoff))) {
    return EFI_NOT_FOUND;
  }

  CopyMem (&Handoff, GET_GUID_HOB_DATA (GuidHob), sizeof (Handoff));
  Info->BootHart = Handoff.BootHartId;
  Cpus           = FdtPathOffset (Fdt, "/cpus");
  if (Cpus < 0) {
    return EFI_COMPROMISED_DATA;
  }

  FdtForEachSubnode (Node, Fdt, Cpus) {
    Value = FdtGetProp (Fdt, Node, "device_type", &Length);
    if (!Value || (Length != 4) || CompareMem (Value, "cpu", 4)) {
      continue;
    }

    Value = FdtGetProp (Fdt, Node, "status", &Length);
    if (!Value || ((Length == 5) && !CompareMem (Value, "okay", 5)) ||
        ((Length == 3) && !CompareMem (Value, "ok", 3)))
    {
      Info->CoreCount++;
    }
  }
  Info->MemoryBase = MAX_UINT64;
  for (Hob = GetHobList (); !END_OF_HOB_LIST (Hob); Hob = GET_NEXT_HOB (Hob)) {
    EFI_HOB_RESOURCE_DESCRIPTOR  *Resource;
    if (Hob->HobType != EFI_HOB_TYPE_RESOURCE_DESCRIPTOR) {
      continue;
    }

    Resource = (VOID *)Hob;
    if (Resource->ResourceType != EFI_RESOURCE_SYSTEM_MEMORY) {
      continue;
    }

    if (!Resource->ResourceLength || (Resource->PhysicalStart > MAX_UINT64 - Resource->ResourceLength) ||
        (Info->MemorySize > MAX_UINT64 - Resource->ResourceLength))
    {
      return EFI_COMPROMISED_DATA;
    }

    Info->MemorySize += Resource->ResourceLength;
    Info->MemoryBase  = MIN (Info->MemoryBase, Resource->PhysicalStart);
  }

  if (!Info->CoreCount || !Info->MemorySize || (Info->MemoryBase > MAX_UINT64 - Info->MemorySize)) {
    return EFI_COMPROMISED_DATA;
  }

  if (DeviceTree != NULL) {
    *DeviceTree = Fdt;
  }
  return EFI_SUCCESS;
}
