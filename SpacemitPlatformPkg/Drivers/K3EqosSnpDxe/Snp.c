/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Guid/RiscVSecHobData.h>
#include <Protocol/SimpleNetwork.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/K3EqosDmaLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PlatformMacAddressLib.h>
#include <Library/PrintLib.h>
#include <Library/SnpAdapterInfoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define SIGNATURE  SIGNATURE_32 ('K','3','S','N')
typedef struct NIC {
  struct NIC                     *Next;
  UINT32                         Signature;
  K3_EQOS_PORT                   Port;
  K3_EQOS_DMA                    Dma;
  UINT64                         FilteredFrames;
  EFI_SIMPLE_NETWORK_PROTOCOL    Snp;
  EFI_SIMPLE_NETWORK_MODE        Mode;
  SNP_ADAPTER_INFO               AdapterInfo;
  EFI_HANDLE                     Handle;
  EFI_EVENT                      Exit;
  struct {
    MAC_ADDR_DEVICE_PATH        Mac;
    EFI_DEVICE_PATH_PROTOCOL    End;
  } Path;
} NIC;
STATIC NIC  *mQuarantined;
#define NIC_FROM(p)  CR (p, NIC, Snp, SIGNATURE)
#define FILTERS  (EFI_SIMPLE_NETWORK_RECEIVE_UNICAST | EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST |\
                 EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST | EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS | \
                 EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST)

STATIC BOOLEAN
ValidMac (
  CONST UINT8  *M
  )
{
  UINT8  Or = 0, And = 0xff;

  for (UINTN I = 0; I < 6; I++) {
    Or  |= M[I];
    And &= M[I];
  }

  return Or && And != 0xff && !(M[0] & 1);
}

STATIC EFI_STATUS
State (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  NIC                          **N
  )
{
  if (!This) {
    return EFI_INVALID_PARAMETER;
  }

  *N = NIC_FROM (This);
  if ((*N)->Mode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  return ((*N)->Mode.State == EfiSimpleNetworkInitialized) &&
         (*N)->Dma.Running && !(*N)->Dma.Faulted ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS
Link (
  NIC  *N
  )
{
  BOOLEAN     Up, Full;
  UINT32      Speed;
  EFI_STATUS  Status = K3EqosLink (&N->Port, &Up, &Full, &Speed);

  N->Mode.MediaPresent = !EFI_ERROR (Status) && Up;
  if (!EFI_ERROR (Status) && Up) {
    K3EqosSpeed (&N->Port, Speed, Full);
  }

  return Status;
}

STATIC VOID
ProgramFilters (
  NIC  *N
  )
{
  UINT32  F = N->Mode.ReceiveFilterSetting, Bits = 0;

  if (!(F & EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST)) {
    Bits |= BIT5;
  }

  if (F & EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS) {
    Bits |= BIT0;
  }

  if (F & (EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST | EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST)) {
    Bits |= BIT4;
  }

  // Multicast is checked exactly in software, avoiding hardware-hash collisions.
  MmioAndThenOr32 (N->Port.Base + 8, ~(BIT0 | BIT1 | BIT2 | BIT4 | BIT5 | BIT10), Bits);
}

STATIC BOOLEAN
Accept (
  NIC          *N,
  CONST UINT8  *M
  )
{
  UINT32  F = N->Mode.ReceiveFilterSetting;

  if (F & EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS) {
    return TRUE;
  }

  BOOLEAN  Broadcast = TRUE;

  for (UINTN I = 0; I < 6; I++) {
    if (M[I] != 0xff) {
      Broadcast = FALSE;
    }
  }

  if (Broadcast) {
    return (F & EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST) != 0;
  }

  if (!(M[0] & 1)) {
    return (F & EFI_SIMPLE_NETWORK_RECEIVE_UNICAST) && !CompareMem (M, &N->Mode.CurrentAddress, 6);
  }

  if (F & EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST) {
    return TRUE;
  }

  if (F & EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST) {
    for (UINTN I = 0; I < N->Mode.MCastFilterCount; I++) {
      if (!CompareMem (M, &N->Mode.MCastFilter[I], 6)) {
        return TRUE;
      }
    }
  }

  return FALSE;
}

STATIC EFI_STATUS EFIAPI
Start (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This
  )
{
  if (!This) {
    return EFI_INVALID_PARAMETER;
  }

  NIC  *N = NIC_FROM (This);

  if (N->Mode.State != EfiSimpleNetworkStopped) {
    return EFI_ALREADY_STARTED;
  }

  N->Mode.State = EfiSimpleNetworkStarted;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
Stop (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This
  )
{
  if (!This) {
    return EFI_INVALID_PARAMETER;
  }

  NIC  *N = NIC_FROM (This);

  if (N->Mode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if ((N->Mode.State == EfiSimpleNetworkInitialized) ||
      N->Dma.Allocation || N->Dma.Running || N->Dma.Faulted)
  {
    return EFI_DEVICE_ERROR;
  }

  N->Mode.State = EfiSimpleNetworkStopped;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
Initialize (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  UINTN                        ExtraRx,
  UINTN                        ExtraTx
  )
{
  if (!This) {
    return EFI_INVALID_PARAMETER;
  }

  NIC  *N = NIC_FROM (This);

  if (N->Mode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if (N->Mode.State == EfiSimpleNetworkInitialized) {
    return EFI_ALREADY_STARTED;
  }

  if (ExtraRx || ExtraTx) {
    return EFI_UNSUPPORTED;
  }

  EFI_TPL     Old    = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  Status = K3EqosDmaStart (&N->Dma, &N->Port, (UINT8 *)&N->Mode.CurrentAddress);

  if (!EFI_ERROR (Status)) {
    N->FilteredFrames = 0;
    ProgramFilters (N);
    N->Mode.State = EfiSimpleNetworkInitialized;
    // A disconnected cable is a usable initialized interface, with MediaPresent false.
    Status = Link (N);
    if (EFI_ERROR (Status)) {
      EFI_STATUS  Cleanup = K3EqosDmaStop (&N->Dma);
      if (!EFI_ERROR (Cleanup)) {
        Cleanup = K3EqosDmaFree (&N->Dma);
      }

      if (!EFI_ERROR (Cleanup)) {
        N->Mode.State = EfiSimpleNetworkStarted;
      }
    }
  }

  gBS->RestoreTPL (Old);
  return Status;
}

STATIC EFI_STATUS EFIAPI
Shutdown (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This
  )
{
  NIC         *N;
  EFI_STATUS  Status;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  N = NIC_FROM (This);
  if (N->Mode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if ((N->Mode.State != EfiSimpleNetworkInitialized) && !N->Dma.Allocation &&
      !N->Dma.Running && !N->Dma.Faulted)
  {
    return EFI_DEVICE_ERROR;
  }

  EFI_TPL  Old = gBS->RaiseTPL (TPL_NOTIFY);

  Status = K3EqosDmaStop (&N->Dma);
  if (!EFI_ERROR (Status)) {
    Status = K3EqosDmaFree (&N->Dma);
  }

  if (!EFI_ERROR (Status)) {
    N->Mode.State        = EfiSimpleNetworkStarted;
    N->Mode.MediaPresent = FALSE;
  }

  gBS->RestoreTPL (Old);
  return Status;
}

STATIC EFI_STATUS EFIAPI
Reset (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  BOOLEAN                      Extended
  )
{
  (VOID)Extended;
  EFI_STATUS  Status = Shutdown (This);

  return EFI_ERROR (Status) ? Status : Initialize (This, 0, 0);
}

STATIC EFI_STATUS EFIAPI
ReceiveFilters (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  UINT32                       Enable,
  UINT32                       Disable,
  BOOLEAN                      Clear,
  UINTN                        Count,
  EFI_MAC_ADDRESS              *Addresses
  )
{
  NIC         *N;
  EFI_STATUS  Status = State (This, &N);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Enable | Disable) & ~FILTERS) {
    return EFI_INVALID_PARAMETER;
  }

  if (!Clear) {
    if ((Count > MAX_MCAST_FILTER_CNT) || (Count && !Addresses) ||
        ((Enable & ~Disable & EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST) && !Count))
    {
      return EFI_INVALID_PARAMETER;
    }

    for (UINTN I = 0; I < Count; I++) {
      if (!(Addresses[I].Addr[0] & 1)) {
        return EFI_INVALID_PARAMETER;
      }
    }
  }

  EFI_TPL  Old = gBS->RaiseTPL (TPL_NOTIFY);

  if (Clear) {
    N->Mode.MCastFilterCount = 0;
    ZeroMem (N->Mode.MCastFilter, sizeof (N->Mode.MCastFilter));
  } else if (Count) {
    CopyMem (N->Mode.MCastFilter, Addresses, Count * sizeof (*Addresses));
    N->Mode.MCastFilterCount = (UINT32)Count;
  }

  N->Mode.ReceiveFilterSetting = (N->Mode.ReceiveFilterSetting | Enable) & ~Disable;
  ProgramFilters (N);
  gBS->RestoreTPL (Old);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
StationAddress (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  BOOLEAN                      Default,
  EFI_MAC_ADDRESS              *Mac
  )
{
  NIC         *N;
  EFI_STATUS  Status = State (This, &N);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Default && (!Mac || !ValidMac (Mac->Addr))) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_TPL  Old = gBS->RaiseTPL (TPL_NOTIFY);

  N->Mode.CurrentAddress = Default ? N->Mode.PermanentAddress : *Mac;
  K3EqosMacAddress (&N->Port, N->Mode.CurrentAddress.Addr);
  gBS->RestoreTPL (Old);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
Statistics (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  BOOLEAN                      Clear,
  UINTN                        *Size,
  EFI_NETWORK_STATISTICS       *Stats
  )
{
  NIC                     *N;
  EFI_STATUS              Status = State (This, &N);
  EFI_NETWORK_STATISTICS  Snapshot;
  EFI_TPL                 Old;
  UINTN                   Capacity;

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Clear && (Size == NULL)) {
    return Stats == NULL ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
  }

  Old = gBS->RaiseTPL (TPL_NOTIFY);
  if (Clear) {
    N->Dma.RxGood     = N->Dma.RxError = N->Dma.TxGood = N->Dma.TxError = 0;
    N->FilteredFrames = 0;
    gBS->RestoreTPL (Old);
    return EFI_SUCCESS;
  }

  // Only supported counters are valid; unavailable statistics are all ones.
  SetMem (&Snapshot, sizeof (Snapshot), 0xff);
  Snapshot.RxGoodFrames    = N->Dma.RxGood - N->FilteredFrames;
  Snapshot.RxDroppedFrames = N->Dma.RxError + N->FilteredFrames;
  Snapshot.RxTotalFrames   = N->Dma.RxGood + N->Dma.RxError;
  Snapshot.TxGoodFrames    = N->Dma.TxGood;
  Snapshot.TxDroppedFrames = N->Dma.TxError;
  Snapshot.TxTotalFrames   = N->Dma.TxGood + N->Dma.TxError;
  Capacity                 = *Size;
  *Size                    = sizeof (Snapshot);
  if (Stats != NULL) {
    // A short response contains complete counters, never a partial UINT64.
    CopyMem (Stats, &Snapshot, MIN (Capacity - Capacity % sizeof (UINT64), sizeof (Snapshot)));
  }

  Status = (Stats == NULL) || (Capacity < sizeof (Snapshot)) ?
           EFI_BUFFER_TOO_SMALL : EFI_SUCCESS;
  gBS->RestoreTPL (Old);
  return Status;
}

STATIC EFI_STATUS EFIAPI
Mcast (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  BOOLEAN                      V6,
  EFI_IP_ADDRESS               *Ip,
  EFI_MAC_ADDRESS              *Mac
  )
{
  NIC         *N;
  EFI_STATUS  Status = State (This, &N);

  (VOID)N;

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Ip || !Mac) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Mac, sizeof (*Mac));
  if (V6) {
    if (Ip->v6.Addr[0] != 0xff) {
      return EFI_INVALID_PARAMETER;
    }

    Mac->Addr[0] = Mac->Addr[1] = 0x33;
    CopyMem (&Mac->Addr[2], &Ip->v6.Addr[12], 4);
  } else {
    if ((Ip->v4.Addr[0] & 0xf0) != 0xe0) {
      return EFI_INVALID_PARAMETER;
    }

    Mac->Addr[0] = 1;
    Mac->Addr[2] = 0x5e;
    Mac->Addr[3] = Ip->v4.Addr[1] & 0x7f;
    Mac->Addr[4] = Ip->v4.Addr[2];
    Mac->Addr[5] = Ip->v4.Addr[3];
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
NvData (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  BOOLEAN                      Read,
  UINTN                        Offset,
  UINTN                        Size,
  VOID                         *Buffer
  )
{
  (VOID)Read;
  (VOID)Offset;
  (VOID)Size;
  (VOID)Buffer;
  NIC         *N;
  EFI_STATUS  Status = State (This, &N);

  (VOID)N;

  return EFI_ERROR (Status) ? Status : EFI_UNSUPPORTED;
}

STATIC EFI_STATUS EFIAPI
GetStatus (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  UINT32                       *Interrupt,
  VOID                         **Buffer
  )
{
  NIC         *N;
  EFI_STATUS  Status = State (This, &N);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  EFI_TPL  Old       = gBS->RaiseTPL (TPL_NOTIFY);
  UINT32   DmaStatus = MmioRead32 (N->Port.Base + 0x1160);

  if (Buffer != NULL) {
    *Buffer = NULL;
  }

  if (DmaStatus & BIT12) {
    N->Dma.Faulted = TRUE;
    Status         = EFI_DEVICE_ERROR;
    goto Done;
  }

  // Do not remove a recyclable buffer before an operation that can fail.
  // MNP only consumes the returned pointer when GetStatus succeeds.
  Status = Link (N);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (Buffer != NULL) {
    Status = K3EqosReclaim (&N->Dma, Buffer);
    if ((Status == EFI_DEVICE_ERROR) && (*Buffer != NULL)) {
      // A completed descriptor can be recycled even if its frame was dropped.
      // The DMA layer has already recorded the error in TxError.
      Status = EFI_SUCCESS;
    }

    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }

  if (Interrupt != NULL) {
    *Interrupt = ((DmaStatus & BIT0) ? EFI_SIMPLE_NETWORK_TRANSMIT_INTERRUPT : 0) |
                 (K3EqosPacketReady (&N->Dma) ? EFI_SIMPLE_NETWORK_RECEIVE_INTERRUPT : 0);
  }

  MmioWrite32 (N->Port.Base + 0x1160, DmaStatus & (BIT0 | BIT6 | BIT12 | BIT14 | BIT15));

Done:
  gBS->RestoreTPL (Old);
  return Status;
}

STATIC EFI_STATUS EFIAPI
Transmit (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  UINTN                        Header,
  UINTN                        Bytes,
  VOID                         *Buffer,
  EFI_MAC_ADDRESS              *Src,
  EFI_MAC_ADDRESS              *Dst,
  UINT16                       *Protocol
  )
{
  NIC         *N;
  EFI_STATUS  Status = State (This, &N);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Buffer || (Bytes < 14) || (Bytes > 1514) || (Header && ((Header != 14) || !Dst || !Protocol))) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_TPL  Old = gBS->RaiseTPL (TPL_NOTIFY);

  Status = Link (N);
  if (!EFI_ERROR (Status) && !N->Mode.MediaPresent) {
    Status = EFI_NOT_READY;
  }

  if (!EFI_ERROR (Status)) {
    if (Header) {
      UINT8  *P = Buffer;
      CopyMem (P, Dst, 6);
      CopyMem (P + 6, Src ? Src : &N->Mode.CurrentAddress, 6);
      P[12] = (UINT8)(*Protocol >> 8);
      P[13] = (UINT8)*Protocol;
    }

    Status = K3EqosTransmit (&N->Dma, Buffer, Bytes);
  }

  gBS->RestoreTPL (Old);
  return Status;
}

STATIC EFI_STATUS EFIAPI
Receive (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  UINTN                        *Header,
  UINTN                        *Bytes,
  VOID                         *Buffer,
  EFI_MAC_ADDRESS              *Src,
  EFI_MAC_ADDRESS              *Dst,
  UINT16                       *Protocol
  )
{
  NIC         *N;
  EFI_STATUS  Status = State (This, &N);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Bytes || !Buffer) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_TPL  Old = gBS->RaiseTPL (TPL_NOTIFY);

  for (UINTN I = 0; I < K3_EQOS_RX; I++) {
    if (!K3EqosPacketReady (&N->Dma)) {
      Status = EFI_NOT_READY;
      break;
    }

    MemoryFence ();
    UINT8  *Packet = N->Dma.RxBytes + N->Dma.RxHead * K3_EQOS_FRAME_BYTES;
    if (!Accept (N, Packet)) {
      UINT8  Discard[K3_EQOS_FRAME_BYTES];
      UINTN  Size = sizeof (Discard);
      if (!EFI_ERROR (K3EqosReceive (&N->Dma, Discard, &Size))) {
        N->FilteredFrames++;
      }

      Status = EFI_NOT_READY;
      continue;
    }

    Status = K3EqosReceive (&N->Dma, Buffer, Bytes);
    if (!EFI_ERROR (Status)) {
      UINT8  *P = Buffer;
      if (Header) {
        *Header = 14;
      }

      if (Dst) {
        ZeroMem (Dst, sizeof (*Dst));
        CopyMem (Dst, P, 6);
      }

      if (Src) {
        ZeroMem (Src, sizeof (*Src));
        CopyMem (Src, P + 6, 6);
      }

      if (Protocol) {
        *Protocol = ((UINT16)P[12] << 8) | P[13];
      }
    }

    break;
  }

  gBS->RestoreTPL (Old);
  return Status;
}

STATIC VOID EFIAPI
WaitPacket (
  EFI_EVENT  Event,
  VOID       *Context
  )
{
  NIC  *N = Context;

  if ((N->Mode.State == EfiSimpleNetworkInitialized) && K3EqosPacketReady (&N->Dma)) {
    gBS->SignalEvent (Event);
  }
}

STATIC VOID EFIAPI
Exit (
  EFI_EVENT  Event,
  VOID       *Context
  )
{
  (VOID)Event;
  NIC  *N = Context;

  if (N->Dma.Running || N->Dma.Faulted) {
    EFI_STATUS  Status = K3EqosDmaStop (&N->Dma);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "K3 EQoS DMA stop failed at OS handoff: %r\n", Status));
      CpuDeadLoop ();
    }
  }
}

STATIC VOID
Discard (
  NIC  *N
  )
{
  if (N->Port.Owned && EFI_ERROR (K3EqosPark (&N->Port))) {
    N->Next      = mQuarantined;
    mQuarantined = N;
    DEBUG ((DEBUG_ERROR, "K3 EQoS resource park failed; owner retained\n"));
    return;
  }

  FreePool (N);
}

STATIC EFI_STATUS
BoardMac (
  CONST VOID  *Fdt,
  INT32       Node,
  UINT8       Mac[6]
  )
{
  INT32  Aliases = FdtPathOffset (Fdt, "/aliases");

  if (Aliases < 0) {
    return EFI_NOT_FOUND;
  }

  for (UINTN Index = 0; Index < 16; Index++) {
    CHAR8        Name[24];
    INT32        Length;
    CONST CHAR8  *Path;

    AsciiSPrint (Name, sizeof (Name), "ethernet%u", Index);
    Path = FdtGetProp (Fdt, Aliases, Name, &Length);
    if (Path && (Length > 1) && (Path[Length - 1] == '\0') &&
        (FdtPathOffset (Fdt, Path) == Node))
    {
      return PlatformGetMacAddress (Index, Mac);
    }
  }

  return EFI_NOT_FOUND;
}

EFI_STATUS EFIAPI
K3EqosEntry (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  (VOID)Image;
  (VOID)SystemTable;
  CONST EFI_GUID          Guid = RISCV_SEC_HANDOFF_HOB_GUID;
  EFI_HOB_GUID_TYPE       *Hob = GetFirstGuidHob (&Guid);
  RISCV_SEC_HANDOFF_DATA  Handoff;

  if (!Hob || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (Handoff))) {
    return EFI_NOT_FOUND;
  }

  CopyMem (&Handoff, GET_GUID_HOB_DATA (Hob), sizeof (Handoff));
  VOID  *Fdt = Handoff.FdtPointer;

  if (!Fdt || FdtCheckHeader (Fdt) || (FdtTotalSize (Fdt) > SIZE_1MB)) {
    return EFI_COMPROMISED_DATA;
  }

  INT32       Node  = -1;
  UINTN       Count = 0;
  EFI_STATUS  Last  = EFI_NOT_FOUND;

  while ((Node = FdtNodeOffsetByCompatible (Fdt, Node, "spacemit,k3-dwmac")) >= 0) {
    if (!K3FdtEnabled (Fdt, Node)) {
      continue;
    }

    NIC  *N = AllocateZeroPool (sizeof (*N));
    if (!N) {
      // Returning an error would unload callbacks already published for a port.
      Last = EFI_OUT_OF_RESOURCES;
      break;
    }

    CONST CHAR8  *Phase = "resource description";

    N->Signature = SIGNATURE;
    Last         = K3EqosParse (Fdt, Node, &N->Port);
    UINT8  Mac[6];
    if (!EFI_ERROR (Last)) {
      Phase = "board MAC";
      Last  = BoardMac (Fdt, Node, Mac);
    }

    if (!EFI_ERROR (Last)) {
      CopyMem (N->Mode.CurrentAddress.Addr, Mac, 6);
      Phase = "controller setup";
      Last  = K3EqosPrepare (&N->Port);
    }

    if (!EFI_ERROR (Last)) {
      Phase = "PHY setup";
      Last  = K3EqosPhyConfigure (&N->Port);
    }

    if (EFI_ERROR (Last)) {
      DEBUG ((DEBUG_ERROR, "K3 EQoS %lx %a failed: %r\n", N->Port.Base, Phase, Last));
      Discard (N);
      continue;
    }

    N->Mode.PermanentAddress      = N->Mode.CurrentAddress;
    N->Mode.HwAddressSize         = 6;
    N->Mode.MediaHeaderSize       = 14;
    N->Mode.MaxPacketSize         = 1500;
    N->Mode.IfType                = 1;
    N->Mode.ReceiveFilterMask     = FILTERS;
    N->Mode.ReceiveFilterSetting  = EFI_SIMPLE_NETWORK_RECEIVE_UNICAST | EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST;
    N->Mode.MaxMCastFilterCount   = MAX_MCAST_FILTER_CNT;
    N->Mode.MacAddressChangeable  = TRUE;
    N->Mode.MultipleTxSupported   = TRUE;
    N->Mode.MediaPresentSupported = TRUE;
    SetMem (N->Mode.BroadcastAddress.Addr, 6, 0xff);
    N->Snp = (EFI_SIMPLE_NETWORK_PROTOCOL) {
      EFI_SIMPLE_NETWORK_PROTOCOL_REVISION, Start, Stop, Initialize, Reset,
      Shutdown, ReceiveFilters, StationAddress, Statistics, Mcast, NvData, GetStatus, Transmit, Receive, NULL, &N->Mode
    };
    N->Path.Mac.Header.Type    = MESSAGING_DEVICE_PATH;
    N->Path.Mac.Header.SubType = MSG_MAC_ADDR_DP;
    SetDevicePathNodeLength (&N->Path.Mac.Header, sizeof (N->Path.Mac));
    N->Path.Mac.MacAddress = N->Mode.CurrentAddress;
    N->Path.Mac.IfType     = 1;
    SetDevicePathEndNode (&N->Path.End);
    Last = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, WaitPacket, N, &N->Snp.WaitForPacket);
    if (!EFI_ERROR (Last)) {
      Last = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, Exit, N, &gEfiEventExitBootServicesGuid, &N->Exit);
    }

    if (!EFI_ERROR (Last)) {
      Last = SnpAdapterInfoInitialize (&N->AdapterInfo, &N->Snp);
    }

    if (!EFI_ERROR (Last)) {
      Last = gBS->InstallMultipleProtocolInterfaces (
                    &N->Handle,
                    &gEfiSimpleNetworkProtocolGuid,
                    &N->Snp,
                    &gEfiAdapterInformationProtocolGuid,
                    &N->AdapterInfo.Protocol,
                    &gEfiDevicePathProtocolGuid,
                    &N->Path,
                    NULL
                    );
    }

    if (EFI_ERROR (Last)) {
      if (N->Exit) {
        gBS->CloseEvent (N->Exit);
      }

      if (N->Snp.WaitForPacket) {
        gBS->CloseEvent (N->Snp.WaitForPacket);
      }

      Discard (N);
      continue;
    }

    Count++;
    DEBUG ((
      DEBUG_INFO,
      "K3 EQoS %lx SNP published, PHY %08x, DMA width %u\n",
      N->Port.Base,
      N->Port.PhyId,
      N->Port.AddressBits
      ));
  }

  return Count ? EFI_SUCCESS : Last;
}
