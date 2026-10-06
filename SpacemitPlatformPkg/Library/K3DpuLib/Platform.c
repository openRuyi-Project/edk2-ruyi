/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/K3DpuLib.h>
#include <Library/K3HardwareLib.h>

STATIC BOOLEAN
Cell (
  CONST VOID   *Fdt,
  INT32        Node,
  CONST CHAR8  *Name,
  UINT32       Value
  )
{
  INT32         Length;
  CONST UINT32  *P = FdtGetProp (Fdt, Node, Name, &Length);

  return P && Length == 4 && Fdt32ToCpu (*P) == Value;
}

STATIC EFI_STATUS
Input (
  CONST VOID  *Fdt,
  INT32       Node,
  INT32       *Endpoint,
  INT32       *Remote,
  INT32       *Peer
  )
{
  INT32  Ports = FdtSubnodeOffset (Fdt, Node, "ports"), Port = -1;

  if ((Ports < 0) || !Cell (Fdt, Ports, "#address-cells", 1) || !Cell (Fdt, Ports, "#size-cells", 0)) {
    return EFI_COMPROMISED_DATA;
  }

  for (INT32 P = FdtFirstSubnode (Fdt, Ports); P >= 0; P = FdtNextSubnode (Fdt, P)) {
    if (!Cell (Fdt, P, "reg", 0)) {
      continue;
    }

    if ((Port >= 0) || !K3FdtEnabled (Fdt, P)) {
      return EFI_COMPROMISED_DATA;
    }

    Port = P;
  }

  if (Port < 0) {
    return EFI_NOT_FOUND;
  }

  *Endpoint = FdtFirstSubnode (Fdt, Port);
  if ((*Endpoint < 0) || (FdtNextSubnode (Fdt, *Endpoint) >= 0) || !K3FdtEnabled (Fdt, *Endpoint)) {
    return EFI_UNSUPPORTED;
  }

  INT32         Length;
  CONST UINT32  *P = FdtGetProp (Fdt, *Endpoint, "remote-endpoint", &Length);

  if (!P || (Length != 4)) {
    return EFI_COMPROMISED_DATA;
  }

  *Remote = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*P));
  INT32  PeerPort = FdtParentOffset (Fdt, *Remote), PeerPorts = FdtParentOffset (Fdt, PeerPort);

  *Peer = FdtParentOffset (Fdt, PeerPorts);
  return *Remote >= 0 && K3FdtEnabled (Fdt, *Remote) && Cell (Fdt, PeerPort, "reg", 0) ?
         EFI_SUCCESS : EFI_COMPROMISED_DATA;
}

EFI_STATUS
K3DpuParse (
  CONST VOID        *Fdt,
  INT32             Node,
  K3_DPU_RESOURCES  *R
  )
{
  if (!R || !Fdt || FdtCheckHeader (Fdt) || !K3FdtEnabled (Fdt, Node) ||
      (FdtStringListSearch (Fdt, Node, "compatible", "spacemit,k3-saturn-dpu") < 0))
  {
    return EFI_NOT_FOUND;
  }

  K3_DPU_RESOURCES  Plan   = { .Fdt = Fdt, .DpuNode = Node };
  EFI_STATUS        Status = K3FdtRegister (Fdt, Node, 0, 0x54000, &Plan.Dpu, NULL);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* DPU1/DP1 external connector; DPU0/eDP panel sequencing is separate. */
  if (Plan.Dpu != 0xc0440000) {
    return EFI_UNSUPPORTED;
  }

  INT32  E, Remote, Peer, Back, BackRemote, BackPeer, Length;

  Status = Input (Fdt, Node, &E, &Remote, &Peer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (FdtStringListSearch (Fdt, Peer, "compatible", "spacemit,k3-inno-dp") < 0) {
    return EFI_UNSUPPORTED;
  }

  Status = Input (Fdt, Peer, &Back, &BackRemote, &BackPeer);
  if (EFI_ERROR (Status) || (BackRemote != E) || (Back != Remote) || (BackPeer != Node)) {
    return EFI_COMPROMISED_DATA;
  }

  Plan.DpNode = Peer;
  Status      = K3FdtRegister (Fdt, Peer, 0, 0x4000, &Plan.Dp, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Plan.Dp != 0xcac88000) {
    return EFI_UNSUPPORTED;
  }

  CONST UINT32  *Clocks = FdtGetProp (Fdt, Node, "clocks", &Length);

  if (!Clocks || (Length != 40) || (FdtStringListCount (Fdt, Node, "clock-names") != 5) ||
      (FdtStringListCount (Fdt, Node, "reset-names") != 5))
  {
    return EFI_COMPROMISED_DATA;
  }

  INT32  Apmu = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Clocks[0]));

  if (!K3FdtEnabled (Fdt, Apmu) || !Cell (Fdt, Apmu, "#clock-cells", 1) ||
      !Cell (Fdt, Apmu, "#reset-cells", 1) ||
      (FdtStringListSearch (Fdt, Apmu, "compatible", "spacemit,k3-syscon-apmu") < 0))
  {
    return EFI_UNSUPPORTED;
  }

  static CONST UINT32  ClockIds[] = { 49, 50, 51, 47, 48 }, ResetIds[] = { 55, 54, 53, 57, 56 };
  static CONST CHAR8   *ClockNames[] = { "pxclk", "mclk", "aclk", "escclk", "dscclk" };
  static CONST CHAR8   *ResetNames[] = { "mclk", "lcd", "esc", "aclk", "dsc" };
  CONST UINT32         *Resets       = FdtGetProp (Fdt, Node, "resets", &Length);

  if (!Resets || (Length != 40)) {
    return EFI_COMPROMISED_DATA;
  }

  for (UINTN I = 0; I < 5; I++) {
    if ((Clocks[I * 2] != Clocks[0]) || (Resets[I * 2] != Clocks[0]) ||
        (Fdt32ToCpu (Clocks[I * 2 + 1]) != ClockIds[I]) || (Fdt32ToCpu (Resets[I * 2 + 1]) != ResetIds[I]) ||
        (FdtStringListSearch (Fdt, Node, "clock-names", ClockNames[I]) != (INT32)I) ||
        (FdtStringListSearch (Fdt, Node, "reset-names", ResetNames[I]) != (INT32)I))
    {
      return EFI_COMPROMISED_DATA;
    }
  }

  Clocks = FdtGetProp (Fdt, Peer, "clocks", &Length);
  if (!Clocks || (Length != 12) || (FdtStringListCount (Fdt, Peer, "clock-names") != 2) ||
      (FdtStringListCount (Fdt, Peer, "phy-names") != 1) ||
      (FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Clocks)) != Apmu) ||
      (Fdt32ToCpu (Clocks[1]) != 55) || (FdtStringListSearch (Fdt, Peer, "clock-names", "pxclk") != 0) ||
      (FdtStringListSearch (Fdt, Peer, "clock-names", "pll") != 1))
  {
    return EFI_COMPROMISED_DATA;
  }

  INT32         Phy = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Clocks[2]));
  CONST UINT32  *P  = FdtGetProp (Fdt, Peer, "phys", &Length);

  if (!P || (Length != 4) || (*P != Clocks[2]) || (FdtParentOffset (Fdt, Phy) != Peer) ||
      !K3FdtEnabled (Fdt, Phy) || !Cell (Fdt, Phy, "#clock-cells", 0) || !Cell (Fdt, Phy, "#phy-cells", 0) ||
      (FdtStringListSearch (Fdt, Phy, "compatible", "spacemit,k3-inno-dp-phy") < 0) ||
      (FdtStringListSearch (Fdt, Peer, "phy-names", "dp") != 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  P = FdtGetProp (Fdt, Peer, "resets", &Length);
  if (!P || (Length != 8) || (FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*P)) != Apmu) ||
      (Fdt32ToCpu (P[1]) != 61))
  {
    return EFI_COMPROMISED_DATA;
  }

  Status = K3FdtRegister (Fdt, Apmu, 0, 0x3f8, &Plan.Apmu, NULL);
  if (!EFI_ERROR (Status)) {
    *R = Plan;
  }

  return Status;
}
