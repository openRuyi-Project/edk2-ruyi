/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DesignWarePciLib.h>
#include <Library/FdtLib.h>

STATIC BOOLEAN
Enabled (
  CONST VOID  *Fdt,
  INT32       Node
  )
{
  INT32        Length;
  CONST CHAR8  *Status = FdtGetProp (Fdt, Node, "status", &Length);

  return Status == NULL || (Length == 5 && !AsciiStrCmp (Status, "okay")) ||
         (Length == 3 && !AsciiStrCmp (Status, "ok"));
}

STATIC BOOLEAN
IdentityBus (
  CONST VOID  *Fdt,
  INT32       Node
  )
{
  INT32  Length;

  if (Node == 0) {
    return TRUE;
  }

  return Node > 0 && FdtGetProp (Fdt, Node, "ranges", &Length) != NULL && Length == 0;
}

STATIC UINT64
Pair (
  CONST UINT32  *Cells
  )
{
  return ((UINT64)Fdt32ToCpu (Cells[0]) << 32) | Fdt32ToCpu (Cells[1]);
}

STATIC EFI_STATUS
RegisterRegion (
  CONST VOID   *Fdt,
  INT32        Node,
  CONST CHAR8  *Name,
  UINTN        Minimum,
  UINTN        *Address,
  UINTN        *Size
  )
{
  INT32         Index, Length, Parent = FdtParentOffset (Fdt, Node);
  CONST UINT32  *Reg;
  UINT64        Base, Bytes;

  if ((Parent < 0) || (FdtAddressCells (Fdt, Parent) != 2) || (FdtSizeCells (Fdt, Parent) != 2)) {
    return EFI_UNSUPPORTED;
  }

  if (!IdentityBus (Fdt, Parent)) {
    return EFI_UNSUPPORTED;
  }

  Index = FdtStringListSearch (Fdt, Node, "reg-names", Name);
  Reg   = FdtGetProp (Fdt, Node, "reg", &Length);
  if ((Index < 0) || (Reg == NULL) || (Length < 0) || Length % 16 || (Index >= Length / 16)) {
    return EFI_COMPROMISED_DATA;
  }

  Base  = Pair (Reg + Index * 4);
  Bytes = Pair (Reg + Index * 4 + 2);
  if (!Base || (Base & 3) || (Bytes < Minimum) || (Base > MAX_UINTN - Bytes)) {
    return EFI_COMPROMISED_DATA;
  }

  *Address = (UINTN)Base;
  if (Size != NULL) {
    *Size = (UINTN)Bytes;
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
PortParse (
  CONST VOID          *Fdt,
  INT32               Node,
  SPACEMIT_PCIE_PORT  *Port
  )
{
  INT32         Length, Child, Root = -1, Provider;
  CONST UINT32  *Cells;
  EFI_STATUS    Status;
  UINT32        Segment;
  UINTN         ProviderBase, ProviderSize;

  ZeroMem (Port, sizeof (*Port));
  Cells = FdtGetProp (Fdt, Node, "linux,pci-domain", &Length);
  if (!Cells || (Length != 4) || ((Segment = Fdt32ToCpu (Cells[0])) > MAX_UINT16)) {
    return EFI_COMPROMISED_DATA;
  }

  Port->Node    = Node;
  Port->Segment = (UINT16)Segment;
  Status        = RegisterRegion (Fdt, Node, "dbi", 4096, &Port->Dbi, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = RegisterRegion (Fdt, Node, "dbi2", 4096, &Port->Dbi2, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = RegisterRegion (Fdt, Node, "atu", 0x800, &Port->Atu, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = RegisterRegion (Fdt, Node, "config", 4096, &Port->Config, &Port->ConfigSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Port->Config & 0xfff) || (Port->ConfigSize & 0xfff)) {
    return EFI_COMPROMISED_DATA;
  }

  Status = RegisterRegion (Fdt, Node, "link", 8, &Port->Link, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Cells = FdtGetProp (Fdt, Node, "spacemit,apmu", &Length);
  if (!Cells || (Length != 8)) {
    return EFI_COMPROMISED_DATA;
  }

  Provider = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Cells[0]));
  // The clock/reset provider is named by phandle, not by a board path.
  CONST UINT32  *Reg;
  INT32         Parent = FdtParentOffset (Fdt, Provider), RegLength;

  if ((Provider < 0) || (Parent < 0) || (FdtAddressCells (Fdt, Parent) != 2) ||
      (FdtSizeCells (Fdt, Parent) != 2) || !Enabled (Fdt, Provider))
  {
    return EFI_COMPROMISED_DATA;
  }

  if (!IdentityBus (Fdt, Parent)) {
    return EFI_UNSUPPORTED;
  }

  Reg = FdtGetProp (Fdt, Provider, "reg", &RegLength);
  if (!Reg || (RegLength < 16) || RegLength % 16) {
    return EFI_COMPROMISED_DATA;
  }

  ProviderBase = (UINTN)Pair (Reg);
  ProviderSize = (UINTN)Pair (Reg + 2);
  UINT32  Offset = Fdt32ToCpu (Cells[1]);

  if (!ProviderBase || (ProviderBase > MAX_UINTN - ProviderSize) || (ProviderSize < 8) ||
      (Offset & 3) || (Offset > ProviderSize - 8))
  {
    return EFI_COMPROMISED_DATA;
  }

  Port->Control = ProviderBase + Offset;
  Cells         = FdtGetProp (Fdt, Node, "num-lanes", &Length);
  if (!Cells || (Length != 4)) {
    return EFI_COMPROMISED_DATA;
  }

  Port->Lanes = Fdt32ToCpu (Cells[0]);
  if ((Port->Lanes != 1) && (Port->Lanes != 2) && (Port->Lanes != 4) && (Port->Lanes != 8)) {
    return EFI_UNSUPPORTED;
  }

  Port->MaxSpeed = 3;
  Cells          = FdtGetProp (Fdt, Node, "max-link-speed", &Length);
  if (Cells) {
    if ((Length != 4) || !Fdt32ToCpu (Cells[0]) || (Fdt32ToCpu (Cells[0]) > 3)) {
      return EFI_UNSUPPORTED;
    }

    Port->MaxSpeed = Fdt32ToCpu (Cells[0]);
  }

  for (Child = FdtFirstSubnode (Fdt, Node); Child >= 0; Child = FdtNextSubnode (Fdt, Child)) {
    CONST CHAR8  *Type = FdtGetProp (Fdt, Child, "device_type", &Length);
    if (!Enabled (Fdt, Child) || !Type || (Length != 4) || AsciiStrCmp (Type, "pci")) {
      continue;
    }

    if (Root >= 0) {
      return EFI_UNSUPPORTED;
    }

    Root = Child;
  }

  if (Root < 0) {
    return EFI_COMPROMISED_DATA;
  }

  Cells = FdtGetProp (Fdt, Root, "bus-range", &Length);
  if (!Cells || (Length != 8) || !Fdt32ToCpu (Cells[0]) || (Fdt32ToCpu (Cells[0]) > 255) ||
      (Fdt32ToCpu (Cells[1]) > 255) || (Fdt32ToCpu (Cells[0]) > Fdt32ToCpu (Cells[1])))
  {
    return EFI_COMPROMISED_DATA;
  }

  Port->BusMin = (UINT8)(Fdt32ToCpu (Cells[0]) - 1);
  Port->BusMax = (UINT8)Fdt32ToCpu (Cells[1]);
  if ((FdtAddressCells (Fdt, Node) != 3) || (FdtSizeCells (Fdt, Node) != 2) ||
      (FdtAddressCells (Fdt, FdtParentOffset (Fdt, Node)) != 2))
  {
    return EFI_UNSUPPORTED;
  }

  Cells = FdtGetProp (Fdt, Node, "ranges", &Length);
  if (!Cells || (Length <= 0) || Length % 28 || (Length / 28 > 3)) {
    return EFI_UNSUPPORTED;
  }

  UINT32  Types = 0;

  for (INT32 Index = 0; Index < Length / 28; Index++) {
    SPACEMIT_PCIE_RANGE  *Range = &Port->Ranges[Port->RangeCount++];
    UINT32               Flags  = Fdt32ToCpu (Cells[Index * 7]);
    UINT32               Type   = (Flags >> 24) & 3;
    if ((Flags & 0x3cffffff) || !Type || (Types & (1U << Type))) {
      return EFI_COMPROMISED_DATA;
    }

    Types       |= 1U << Type;
    Range->Flags = Flags;
    Range->Pci   = Pair (Cells + Index * 7 + 1);
    Range->Cpu   = Pair (Cells + Index * 7 + 3);
    Range->Size  = Pair (Cells + Index * 7 + 5);
    if (!Range->Size || (Range->Cpu > MAX_UINT64 - Range->Size) || (Range->Pci > MAX_UINT64 - Range->Size) ||
        (Range->Cpu & 0xfff) || (Range->Pci & 0xfff) || (Range->Size & 0xfff) ||
        ((Type == 2) && (Range->Pci + Range->Size > SIZE_4GB)) ||
        ((Type == 1) && (Flags & BIT30)))
    {
      return EFI_COMPROMISED_DATA;
    }

    for (UINT32 Previous = 0; Previous < (UINT32)Index; Previous++) {
      SPACEMIT_PCIE_RANGE  *Other = &Port->Ranges[Previous];
      if ((Range->Cpu < Other->Cpu + Other->Size) && (Other->Cpu < Range->Cpu + Range->Size)) {
        return EFI_COMPROMISED_DATA;
      }
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
SpacemitPcieParse (
  CONST VOID          *Fdt,
  SPACEMIT_PCIE_PORT  *Ports,
  UINTN               Capacity,
  UINTN               *Count
  )
{
  INT32       Node = -1;
  EFI_STATUS  Status;

  if (!Fdt || !Ports || !Count || (Capacity == 0) || (Capacity > SPACEMIT_PCIE_MAX_PORTS) || FdtCheckHeader (Fdt)) {
    return EFI_INVALID_PARAMETER;
  }

  *Count = 0;
  while ((Node = FdtNodeOffsetByCompatible (Fdt, Node, "spacemit,k3-pcie")) >= 0) {
    if (!Enabled (Fdt, Node) || !Enabled (Fdt, FdtParentOffset (Fdt, Node))) {
      continue;
    }

    if (*Count >= Capacity) {
      Status = EFI_OUT_OF_RESOURCES;
      goto Fail;
    }

    Status = PortParse (Fdt, Node, &Ports[*Count]);
    if (EFI_ERROR (Status)) {
      goto Fail;
    }

    for (UINTN I = 0; I < *Count; I++) {
      if ((Ports[I].Segment == Ports[*Count].Segment) || (Ports[I].Dbi == Ports[*Count].Dbi) ||
          (Ports[I].Atu == Ports[*Count].Atu) || (Ports[I].Config == Ports[*Count].Config))
      {
        Status = EFI_COMPROMISED_DATA;
        goto Fail;
      }

      for (UINT32 A = 0; A < Ports[I].RangeCount; A++) {
        for (UINT32 B = 0; B < Ports[*Count].RangeCount; B++) {
          SPACEMIT_PCIE_RANGE  *Old = &Ports[I].Ranges[A], *New = &Ports[*Count].Ranges[B];
          if ((Old->Cpu < New->Cpu + New->Size) && (New->Cpu < Old->Cpu + Old->Size)) {
            Status = EFI_COMPROMISED_DATA;
            goto Fail;
          }
        }
      }
    }

    (*Count)++;
  }

  return EFI_SUCCESS;
Fail:
  ZeroMem (Ports, Capacity * sizeof (*Ports));
  *Count = 0;
  return Status;
}
