/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Guid/FdtHob.h>
#include <Guid/RiscVSecHobData.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/Fu740InfoLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>

STATIC UINT64
PllRate (
  UINT32  Config,
  UINT32  Reference
  )
{
  if (Config & BIT24) {
    return Reference;
  }

  return DivU64x32 (
           MultU64x32 (Reference, 2 * (((Config >> 6) & 0x1ff) + 1)),
           ((Config & 0x3f) + 1) * (1U << ((Config >> 15) & 7))
           );
}

STATIC UINT64
CoreRate (
  CONST VOID  *Fdt
  )
{
  CONST UINT32                     *Data;
  INT32                            Node, Clock, Length;
  UINT64                           Base;
  UINT32                           Reference;
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;

  Node = FdtNodeOffsetByCompatible (Fdt, -1, "sifive,fu740-c000-prci");
  if (Node < 0) {
    return 0;
  }

  Data = FdtGetProp (Fdt, Node, "reg", &Length);
  if (!Data || (Length != 16)) {
    return 0;
  }

  Base = LShiftU64 (Fdt32ToCpu (Data[0]), 32) | Fdt32ToCpu (Data[1]);
  if ((Base & EFI_PAGE_MASK) || Data[2] || (Fdt32ToCpu (Data[3]) < EFI_PAGE_SIZE)) {
    return 0;
  }

  Data = FdtGetProp (Fdt, Node, "clocks", &Length);
  if (!Data || (Length < 4)) {
    return 0;
  }

  Clock = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Data));
  if (Clock < 0) {
    return 0;
  }

  Data = FdtGetProp (Fdt, Clock, "clock-frequency", &Length);
  if (!Data || (Length != 4) || !*Data) {
    return 0;
  }

  Reference = Fdt32ToCpu (*Data);
  Status    = gDS->GetMemorySpaceDescriptor (Base, &Descriptor);
  if (EFI_ERROR (Status) || (Descriptor.GcdMemoryType == EfiGcdMemoryTypeNonExistent)) {
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Base, EFI_PAGE_SIZE, EFI_MEMORY_UC);
    if (EFI_ERROR (Status)) {
      return 0;
    }

    Status = gDS->SetMemorySpaceAttributes (Base, EFI_PAGE_SIZE, EFI_MEMORY_UC);
    if (EFI_ERROR (Status)) {
      return 0;
    }
  } else if ((Descriptor.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) ||
             (Descriptor.BaseAddress > Base) || (Descriptor.Length < EFI_PAGE_SIZE) ||
             (Base - Descriptor.BaseAddress > Descriptor.Length - EFI_PAGE_SIZE))
  {
    return 0;
  }

  if (MmioRead32 (Base + 0x24) & BIT0) {
    return Reference;
  }

  return PllRate (MmioRead32 (Base + ((MmioRead32 (Base + 0x40) & BIT0) ? 0x38 : 0x04)), Reference);
}

EFI_STATUS
Fu740GetBoardInfo (
  FU740_BOARD_INFO  *Info
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

  Info->CpuRate = CoreRate (Fdt);
  return EFI_SUCCESS;
}
