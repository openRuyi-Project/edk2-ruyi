/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/Fu740InfoLib.h>
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
  CONST VOID  *Fdt;
  EFI_STATUS  Status;

  Status = FdtBoardInfoGet (Info, &Fdt);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Info->CpuRate = CoreRate (Fdt);
  return EFI_SUCCESS;
}
