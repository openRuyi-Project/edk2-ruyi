/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiPei.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>

STATIC
INT32
Cells (
  IN CONST VOID   *Fdt,
  IN INT32        Node,
  IN CONST CHAR8  *Name
  )
{
  CONST UINT32  *Prop;
  INT32         Len;

  Prop = FdtGetProp (Fdt, Node, Name, &Len);
  if ((Prop == NULL) || (Len != sizeof (*Prop))) {
    return -1;
  }

  Len = (INT32)Fdt32ToCpu (*Prop);
  return ((Len == 1) || (Len == 2)) ? Len : -1;
}

STATIC
UINT64
Number (
  IN CONST UINT32  *Prop,
  IN INT32         Count
  )
{
  UINT64  Value = 0;

  while (Count-- > 0) {
    Value = (Value << 32) | Fdt32ToCpu (*Prop++);
  }

  return Value;
}

STATIC
EFI_STATUS
Reserve (
  IN UINT64           Base,
  IN UINT64           Size,
  IN EFI_MEMORY_TYPE  Type
  )
{
  UINT64  End;

  if (Size == 0) {
    return EFI_SUCCESS;
  }

  if ((Base > MAX_UINT64 - Size) || (Base + Size > MAX_UINT64 - EFI_PAGE_MASK)) {
    return EFI_COMPROMISED_DATA;
  }

  End   = ALIGN_VALUE (Base + Size, EFI_PAGE_SIZE);
  Base &= ~(UINT64)EFI_PAGE_MASK;
  BuildMemoryAllocationHob (Base, End - Base, Type);
  return EFI_SUCCESS;
}

EFI_STATUS
PublishMemory (
  IN CONST VOID  *Fdt,
  IN UINT64      WorkspaceBase,
  IN UINT64      WorkspaceSize
  )
{
  CONST CHAR8   *Type;
  CONST UINT32  *Reg;
  INT32         Node, Child, AddressCells, SizeCells, Len, Index, Stride;
  UINT64        Base, Size;
  BOOLEAN       HasWorkspace = FALSE;
  EFI_STATUS    Status;

  AddressCells = Cells (Fdt, 0, "#address-cells");
  SizeCells    = Cells (Fdt, 0, "#size-cells");
  if ((AddressCells < 0) || (SizeCells < 0) ||
      (WorkspaceBase > MAX_UINT64 - WorkspaceSize))
  {
    return EFI_COMPROMISED_DATA;
  }

  Stride = AddressCells + SizeCells;
  FdtForEachSubnode (Node, Fdt, 0) {
    Type = FdtGetProp (Fdt, Node, "device_type", &Len);
    if ((Type == NULL) || (Len != sizeof ("memory")) ||
        (AsciiStrCmp (Type, "memory") != 0))
    {
      continue;
    }

    Reg = FdtGetProp (Fdt, Node, "reg", &Len);
    if ((Reg == NULL) || (Len <= 0) || ((Len % (Stride * sizeof (*Reg))) != 0)) {
      return EFI_COMPROMISED_DATA;
    }

    for (Index = 0; Index < Len / sizeof (*Reg); Index += Stride) {
      Base = Number (Reg + Index, AddressCells);
      Size = Number (Reg + Index + AddressCells, SizeCells);
      if ((Size == 0) || (Base > MAX_UINT64 - Size) ||
          ((Base | Size) & EFI_PAGE_MASK))
      {
        return EFI_COMPROMISED_DATA;
      }

      DEBUG ((DEBUG_INFO, "FDT RAM: %lx + %lx\n", Base, Size));
      // Uniform RAM capabilities keep freed image ranges allocatable
      // when GetMemoryMap merges adjacent descriptors.
      BuildResourceDescriptorHob (
        EFI_RESOURCE_SYSTEM_MEMORY,
        EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
        EFI_RESOURCE_ATTRIBUTE_TESTED | EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE |
        EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |
        EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |
        EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE |
        EFI_RESOURCE_ATTRIBUTE_READ_ONLY_PROTECTABLE |
        EFI_RESOURCE_ATTRIBUTE_EXECUTION_PROTECTABLE,
        Base,
        Size
        );
      if ((WorkspaceBase >= Base) &&
          (WorkspaceBase + WorkspaceSize <= Base + Size))
      {
        HasWorkspace = TRUE;
      }
    }
  }
  if (!HasWorkspace) {
    return EFI_UNSUPPORTED;
  }

  Len = FdtGetNumberOfReserveMapEntries (Fdt);
  if (Len < 0) {
    return EFI_COMPROMISED_DATA;
  }

  for (Index = 0; Index < Len; Index++) {
    if (FdtGetReserveMapEntry (Fdt, Index, &Base, &Size) != 0) {
      return EFI_COMPROMISED_DATA;
    }

    Status = Reserve (Base, Size, EfiReservedMemoryType);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Node = FdtPathOffset (Fdt, "/reserved-memory");
  if (Node < 0) {
    return EFI_SUCCESS;
  }

  AddressCells = Cells (Fdt, Node, "#address-cells");
  SizeCells    = Cells (Fdt, Node, "#size-cells");
  if ((AddressCells < 0) || (SizeCells < 0)) {
    return EFI_COMPROMISED_DATA;
  }

  Stride = AddressCells + SizeCells;
  FdtForEachSubnode (Child, Fdt, Node) {
    Reg = FdtGetProp (Fdt, Child, "reg", &Len);
    if (Reg == NULL) {
      continue;
    }

    if ((Len <= 0) || ((Len % (Stride * sizeof (*Reg))) != 0)) {
      return EFI_COMPROMISED_DATA;
    }

    for (Index = 0; Index < Len / sizeof (*Reg); Index += Stride) {
      Base   = Number (Reg + Index, AddressCells);
      Size   = Number (Reg + Index + AddressCells, SizeCells);
      Status = Reserve (
                 Base,
                 Size,
                 FdtGetProp (Fdt, Child, "no-map", NULL) != NULL ?
                 EfiReservedMemoryType : EfiBootServicesData
                 );
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }
  return EFI_SUCCESS;
}
