/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>

BOOLEAN
FdtNodeEnabled (
  CONST VOID  *Fdt,
  INT32       Node
  )
{
  INT32        Length;
  CONST CHAR8  *Status;

  while (Node >= 0) {
    Status = FdtGetProp (Fdt, Node, "status", &Length);
    if (Status && !(((Length == 5) && !AsciiStrCmp (Status, "okay")) ||
                    ((Length == 3) && !AsciiStrCmp (Status, "ok"))))
    {
      return FALSE;
    }

    if (Node == 0) {
      return TRUE;
    }

    Node = FdtParentOffset (Fdt, Node);
  }

  return FALSE;
}

EFI_STATUS
FdtGetMmio (
  CONST VOID  *Fdt,
  INT32       Node,
  UINTN       Index,
  UINTN       Minimum,
  UINTN       *Base,
  UINTN       *Bytes
  )
{
  INT32         Parent, NextParent, Length;
  CONST UINT32  *Reg, *Ranges;
  UINT64        Address, Size, Child, Upstream, Span;
  UINTN         Entry;
  BOOLEAN       Found;

  if (!Fdt || (Node < 0) || !Base) {
    return EFI_INVALID_PARAMETER;
  }

  Parent = FdtParentOffset (Fdt, Node);
  if ((Parent < 0) || (FdtAddressCells (Fdt, Parent) != 2) || (FdtSizeCells (Fdt, Parent) != 2)) {
    return EFI_UNSUPPORTED;
  }

  Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  if (!Reg || (Length < 0) || Length % 16 || (Index >= (UINTN)Length / 16)) {
    return EFI_COMPROMISED_DATA;
  }

  Reg    += Index * 4;
  Address = ((UINT64)Fdt32ToCpu (Reg[0]) << 32) | Fdt32ToCpu (Reg[1]);
  Size    = ((UINT64)Fdt32ToCpu (Reg[2]) << 32) | Fdt32ToCpu (Reg[3]);
  if (!Address || (Address & 3) || (Size < Minimum) || (Address > MAX_UINTN - Size)) {
    return EFI_COMPROMISED_DATA;
  }

  // Translate each parent bus, including explicit identity ranges.
  while (Parent != 0) {
    NextParent = FdtParentOffset (Fdt, Parent);
    if ((NextParent < 0) || (FdtAddressCells (Fdt, Parent) != 2) ||
        (FdtAddressCells (Fdt, NextParent) != 2) || (FdtSizeCells (Fdt, Parent) != 2))
    {
      return EFI_UNSUPPORTED;
    }

    Ranges = FdtGetProp (Fdt, Parent, "ranges", &Length);
    if (!Ranges || (Length < 0) || (Length % (6 * sizeof (UINT32)))) {
      return EFI_UNSUPPORTED;
    }
    if (Length != 0) {
      Found = FALSE;
      for (Entry = 0; Entry < (UINTN)Length / sizeof (UINT32); Entry += 6) {
        Child    = LShiftU64 (Fdt32ToCpu (Ranges[Entry]), 32) | Fdt32ToCpu (Ranges[Entry + 1]);
        Upstream = LShiftU64 (Fdt32ToCpu (Ranges[Entry + 2]), 32) | Fdt32ToCpu (Ranges[Entry + 3]);
        Span     = LShiftU64 (Fdt32ToCpu (Ranges[Entry + 4]), 32) | Fdt32ToCpu (Ranges[Entry + 5]);
        if ((Address < Child) || (Address - Child >= Span) ||
            (Size > Span - (Address - Child)))
        {
          continue;
        }
        if ((Upstream > MAX_UINTN - (Address - Child)) ||
            (Upstream + (Address - Child) > MAX_UINTN - Size))
        {
          return EFI_COMPROMISED_DATA;
        }
        Address = Upstream + (Address - Child);
        Found = TRUE;
        break;
      }
      if (!Found) {
        return EFI_NOT_FOUND;
      }
    }
    Parent = NextParent;
  }
  if (Address & 3) {
    return EFI_UNSUPPORTED;
  }

  *Base = (UINTN)Address;
  if (Bytes) {
    *Bytes = (UINTN)Size;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
FdtMapMmio (
  UINTN  Base,
  UINTN  Bytes
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;
  UINT64                           Start, End, Chunk;

  if (!Bytes || (Bytes > MAX_UINTN - EFI_PAGE_MASK) || (Base > MAX_UINTN - Bytes - EFI_PAGE_MASK)) {
    return EFI_INVALID_PARAMETER;
  }

  Start = Base & ~(UINTN)EFI_PAGE_MASK;
  End   = ALIGN_VALUE (Base + Bytes, EFI_PAGE_SIZE);
  while (Start < End) {
    Status = gDS->GetMemorySpaceDescriptor (Start, &Descriptor);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (!Descriptor.Length || (Descriptor.BaseAddress > Start) ||
        (Descriptor.BaseAddress > MAX_UINT64 - Descriptor.Length) ||
        (Descriptor.BaseAddress + Descriptor.Length <= Start))
    {
      return EFI_COMPROMISED_DATA;
    }

    Chunk = MIN (End, Descriptor.BaseAddress + Descriptor.Length) - Start;
    if ((Start | Chunk) & EFI_PAGE_MASK) {
      return EFI_UNSUPPORTED;
    }

    if (Descriptor.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
      Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Start, Chunk, EFI_MEMORY_UC);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    } else if (Descriptor.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) {
      return EFI_ACCESS_DENIED;
    }

    // A boot consumer may share a register page with a runtime driver.
    Status = gDS->SetMemorySpaceAttributes (
                    Start,
                    Chunk,
                    EFI_MEMORY_UC | (Descriptor.Attributes & EFI_MEMORY_RUNTIME)
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Start += Chunk;
  }

  return EFI_SUCCESS;
}
