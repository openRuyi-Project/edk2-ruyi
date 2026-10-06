/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/Rtl8127PacketsLib.h>
#include <Library/Rtl8127SnpLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define SIGNATURE  SIGNATURE_32('R','2','7','N')
#define FILTERS    (EFI_SIMPLE_NETWORK_RECEIVE_UNICAST |\
                 EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST | \
                 EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST | \
                 EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS | \
                 EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST)

typedef struct {
  UINT32                         Signature;
  EFI_PCI_IO_PROTOCOL            *Pci;
  RTL8127_PACKETS                *Packets;
  EFI_SIMPLE_NETWORK_PROTOCOL    Snp;
  EFI_SIMPLE_NETWORK_MODE        Mode;
  EFI_NETWORK_STATISTICS         Stats;
  VOID                           *TxBuffer;
  BOOLEAN                        TxCompleted;
  BOOLEAN                        Faulted;
  BOOLEAN                        Ended;
  UINT32                         Interrupts;
  UINTN                          RxBytes;
  UINT8                          RxBuffer[2048];
} NIC;

STATIC NIC *
From (
  EFI_SIMPLE_NETWORK_PROTOCOL  *Snp
  )
{
  if (!Snp) {
    return NULL;
  }

  NIC  *N = BASE_CR (Snp, NIC, Snp);

  return N->Signature == SIGNATURE ? N : NULL;
}

STATIC BOOLEAN
ValidMac (
  CONST UINT8  *Mac
  )
{
  UINT8  Any = 0;

  for (UINTN I = 0; I < 6; I++) {
    Any |= Mac[I];
  }

  return Any && !(Mac[0] & 1);
}

STATIC EFI_STATUS
State (
  NIC  *N
  )
{
  if (!N) {
    return EFI_INVALID_PARAMETER;
  }

  if (N->Ended || N->Faulted) {
    return EFI_DEVICE_ERROR;
  }

  if (N->Mode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  return N->Mode.State == EfiSimpleNetworkInitialized ?
         EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS
Link (
  NIC  *N
  )
{
  EFI_STATUS  Status = Rtl8127PacketsLink (
                         N->Packets,
                         &N->Mode.MediaPresent
                         );

  if (EFI_ERROR (Status)) {
    N->Mode.MediaPresent = FALSE;
  }

  return Status;
}

STATIC BOOLEAN
Broadcast (
  CONST UINT8  *Mac
  )
{
  for (UINTN I = 0; I < 6; I++) {
    if (Mac[I] != 0xff) {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC BOOLEAN
Accept (
  NIC          *N,
  CONST UINT8  *Destination
  )
{
  UINT32  Filters = N->Mode.ReceiveFilterSetting;

  if (Filters & EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS) {
    return TRUE;
  }

  if (Broadcast (Destination)) {
    return (Filters & EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST) != 0;
  }

  if (!(Destination[0] & 1)) {
    return (Filters & EFI_SIMPLE_NETWORK_RECEIVE_UNICAST) &&
           !CompareMem (Destination, N->Mode.CurrentAddress.Addr, 6);
  }

  if (Filters & EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST) {
    return TRUE;
  }

  if (Filters & EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST) {
    for (UINTN I = 0; I < N->Mode.MCastFilterCount; I++) {
      if (!CompareMem (Destination, N->Mode.MCastFilter[I].Addr, 6)) {
        return TRUE;
      }
    }
  }

  return FALSE;
}

STATIC EFI_STATUS
ProgramFilters (
  NIC     *N,
  UINT32  Filters
  )
{
  EFI_STATUS  Status = Rtl8127PacketsFilters (
                         N->Packets,
                         (Filters & EFI_SIMPLE_NETWORK_RECEIVE_UNICAST) != 0,
                         (Filters & EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST) != 0,
                         (Filters & (EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST |
                                     EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST)) != 0,
                         (Filters & EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS) != 0
                         );

  if (EFI_ERROR (Status)) {
    N->Faulted = TRUE;
  }

  return Status;
}

STATIC EFI_STATUS
PollRx (
  NIC  *N
  )
{
  if (N->RxBytes) {
    return EFI_SUCCESS;
  }

  for (UINTN I = 0; I < 16; I++) {
    UINTN       Bytes  = sizeof (N->RxBuffer);
    EFI_STATUS  Status = Rtl8127PacketsReceive (
                           N->Packets,
                           N->RxBuffer,
                           &Bytes
                           );
    if (EFI_ERROR (Status)) {
      if (Status == EFI_DEVICE_ERROR) {
        N->Stats.RxTotalFrames++;
        N->Stats.RxDroppedFrames++;
      }

      return Status;
    }

    N->Stats.RxTotalFrames++;
    N->Stats.RxTotalBytes += Bytes;
    if ((Bytes < 14) || (Bytes > 1514) || !Accept (N, N->RxBuffer)) {
      N->Stats.RxDroppedFrames++;
      continue;
    }

    N->Stats.RxGoodFrames++;
    if (Broadcast (N->RxBuffer)) {
      N->Stats.RxBroadcastFrames++;
    } else if (N->RxBuffer[0] & 1) {
      N->Stats.RxMulticastFrames++;
    } else {
      N->Stats.RxUnicastFrames++;
    }

    N->RxBytes = Bytes;
    return EFI_SUCCESS;
  }

  return EFI_NOT_READY;
}

STATIC EFI_STATUS
PollTx (
  NIC  *N
  )
{
  if (!N->TxBuffer || N->TxCompleted) {
    return EFI_SUCCESS;
  }

  EFI_STATUS  Status = Rtl8127PacketsTxDone (N->Packets);

  if (Status == EFI_NOT_READY) {
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status)) {
    N->Faulted = TRUE;
    return Status;
  }

  N->TxCompleted = TRUE;
  N->Stats.TxGoodFrames++;
  N->Interrupts |= EFI_SIMPLE_NETWORK_TRANSMIT_INTERRUPT;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
Start (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This
  )
{
  NIC  *N = From (This);

  if (!N) {
    return EFI_INVALID_PARAMETER;
  }

  if (N->Ended || N->Faulted) {
    return EFI_DEVICE_ERROR;
  }

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
  NIC  *N = From (This);

  if (!N) {
    return EFI_INVALID_PARAMETER;
  }

  if (N->Mode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if (N->Ended || N->Packets ||
      (N->Mode.State != EfiSimpleNetworkStarted))
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
  NIC  *N = From (This);

  if (!N) {
    return EFI_INVALID_PARAMETER;
  }

  if (N->Ended || N->Faulted) {
    return EFI_DEVICE_ERROR;
  }

  if (N->Mode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if (N->Mode.State == EfiSimpleNetworkInitialized) {
    return EFI_ALREADY_STARTED;
  }

  if (N->Packets) {
    return EFI_DEVICE_ERROR;
  }

  if (ExtraRx || ExtraTx) {
    return EFI_UNSUPPORTED;
  }

  EFI_TPL     Old    = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  Status = Rtl8127PacketsOpen (
                         N->Pci,
                         N->Mode.CurrentAddress.Addr,
                         &N->Packets
                         );

  if (!EFI_ERROR (Status)) {
    Status = ProgramFilters (N, N->Mode.ReceiveFilterSetting);
  }

  if (!EFI_ERROR (Status)) {
    Status = Link (N);
  }

  if (!EFI_ERROR (Status)) {
    N->Mode.State = EfiSimpleNetworkInitialized;
  } else {
    EFI_STATUS  Cleanup = Rtl8127PacketsClose (&N->Packets);
    if (EFI_ERROR (Cleanup)) {
      N->Faulted = TRUE;
      Status     = Cleanup;
    } else {
      N->Faulted = FALSE;
    }
  }

  gBS->RestoreTPL (Old);
  return Status;
}

STATIC EFI_STATUS
Close (
  NIC  *N
  )
{
  EFI_STATUS  Status = Rtl8127PacketsClose (&N->Packets);

  if (!EFI_ERROR (Status)) {
    N->Mode.State        = EfiSimpleNetworkStarted;
    N->Mode.MediaPresent = FALSE;
    N->TxBuffer          = NULL;
    N->TxCompleted       = FALSE;
    N->RxBytes           = 0;
    N->Interrupts        = 0;
    N->Faulted           = FALSE;
  }

  return Status;
}

STATIC EFI_STATUS EFIAPI
Shutdown (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This
  )
{
  NIC  *N = From (This);

  if (!N) {
    return EFI_INVALID_PARAMETER;
  }

  if (N->Ended) {
    return EFI_DEVICE_ERROR;
  }

  if (N->Mode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  if ((N->Mode.State != EfiSimpleNetworkInitialized) && !N->Packets) {
    return EFI_DEVICE_ERROR;
  }

  EFI_TPL     Old    = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  Status = Close (N);

  gBS->RestoreTPL (Old);
  return Status;
}

STATIC EFI_STATUS EFIAPI
Reset (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  BOOLEAN                      Extended
  )
{
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
  NIC         *N     = From (This);
  EFI_STATUS  Status = State (N);

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
      if (!(Addresses[I].Addr[0] & 1) || Broadcast (Addresses[I].Addr)) {
        return EFI_INVALID_PARAMETER;
      }
    }
  }

  EFI_TPL  Old     = gBS->RaiseTPL (TPL_NOTIFY);
  UINT32   Setting = (N->Mode.ReceiveFilterSetting | Enable) & ~Disable;

  Status = ProgramFilters (N, Setting);
  if (!EFI_ERROR (Status)) {
    N->Mode.ReceiveFilterSetting = Setting;
    N->Mode.MCastFilterCount     = Clear ? 0 : (UINT32)Count;
    ZeroMem (N->Mode.MCastFilter, sizeof (N->Mode.MCastFilter));
    if (!Clear && Count) {
      CopyMem (N->Mode.MCastFilter, Addresses, Count * sizeof (*Addresses));
    }

    if (N->RxBytes && !Accept (N, N->RxBuffer)) {
      N->RxBytes = 0;
      N->Stats.RxDroppedFrames++;
    }
  }

  gBS->RestoreTPL (Old);
  return Status;
}

STATIC EFI_STATUS EFIAPI
StationAddress (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  BOOLEAN                      Default,
  EFI_MAC_ADDRESS              *Mac
  )
{
  NIC         *N     = From (This);
  EFI_STATUS  Status = State (N);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Default && (!Mac || !ValidMac (Mac->Addr))) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_TPL  Old = gBS->RaiseTPL (TPL_NOTIFY);

  if (N->TxBuffer) {
    Status = EFI_NOT_READY;
  } else {
    EFI_MAC_ADDRESS  Address;
    ZeroMem (&Address, sizeof (Address));
    CopyMem (
      Address.Addr,
      Default ? N->Mode.PermanentAddress.Addr :
      Mac->Addr,
      6
      );
    Status = Rtl8127PacketsAddress (N->Packets, Address.Addr);
    if (!EFI_ERROR (Status)) {
      N->Mode.CurrentAddress = Address;
      if (N->RxBytes && !Accept (N, N->RxBuffer)) {
        N->RxBytes = 0;
        N->Stats.RxDroppedFrames++;
      }
    } else if (Status != EFI_NOT_READY) {
      N->Faulted = TRUE;
    }
  }

  gBS->RestoreTPL (Old);
  return Status;
}

STATIC EFI_STATUS EFIAPI
Statistics (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  BOOLEAN                      Clear,
  UINTN                        *Size,
  EFI_NETWORK_STATISTICS       *Stats
  )
{
  NIC                     *N     = From (This);
  EFI_STATUS              Status = State (N);
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
    ZeroMem (&N->Stats, sizeof (N->Stats));
    gBS->RestoreTPL (Old);
    return EFI_SUCCESS;
  }

  // Only supported counters are valid; unavailable statistics are all ones.
  SetMem (&Snapshot, sizeof (Snapshot), 0xff);
  Snapshot.RxTotalFrames     = N->Stats.RxTotalFrames;
  Snapshot.RxDroppedFrames   = N->Stats.RxDroppedFrames;
  Snapshot.RxGoodFrames      = N->Stats.RxGoodFrames;
  Snapshot.RxTotalBytes      = N->Stats.RxTotalBytes;
  Snapshot.RxBroadcastFrames = N->Stats.RxBroadcastFrames;
  Snapshot.RxMulticastFrames = N->Stats.RxMulticastFrames;
  Snapshot.RxUnicastFrames   = N->Stats.RxUnicastFrames;
  Snapshot.TxTotalFrames     = N->Stats.TxTotalFrames;
  Snapshot.TxGoodFrames      = N->Stats.TxGoodFrames;
  Snapshot.TxTotalBytes      = N->Stats.TxTotalBytes;
  Snapshot.TxBroadcastFrames = N->Stats.TxBroadcastFrames;
  Snapshot.TxMulticastFrames = N->Stats.TxMulticastFrames;
  Snapshot.TxUnicastFrames   = N->Stats.TxUnicastFrames;
  Capacity                   = *Size;
  *Size                      = sizeof (Snapshot);
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
  BOOLEAN                      Ipv6,
  EFI_IP_ADDRESS               *Ip,
  EFI_MAC_ADDRESS              *Mac
  )
{
  NIC         *N     = From (This);
  EFI_STATUS  Status = State (N);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Ip || !Mac || (Ipv6 ? (Ip->v6.Addr[0] != 0xff) :
                      ((Ip->v4.Addr[0] & 0xf0) != 0xe0)))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Mac, sizeof (*Mac));
  if (Ipv6) {
    Mac->Addr[0] = 0x33;
    Mac->Addr[1] = 0x33;
    CopyMem (Mac->Addr + 2, Ip->v6.Addr + 12, 4);
  } else {
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
  UINTN                        Bytes,
  VOID                         *Buffer
  )
{
  EFI_STATUS  Status = State (From (This));

  return EFI_ERROR (Status) ? Status : EFI_UNSUPPORTED;
}

STATIC EFI_STATUS EFIAPI
GetStatus (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  UINT32                       *Interrupt,
  VOID                         **Buffer
  )
{
  NIC         *N     = From (This);
  EFI_STATUS  Status = State (N);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  EFI_TPL  Old = gBS->RaiseTPL (TPL_NOTIFY);

  if (Buffer) {
    *Buffer = NULL;
  }

  Status = PollTx (N);
  if (!EFI_ERROR (Status)) {
    EFI_STATUS  Rx = PollRx (N);
    if ((Rx != EFI_NOT_READY) && EFI_ERROR (Rx)) {
      Status = Rx;
    }
  }

  if (!EFI_ERROR (Status)) {
    Status = Link (N);
  }

  if (!EFI_ERROR (Status)) {
    if (Interrupt) {
      *Interrupt = N->Interrupts |
                   (N->RxBytes ? EFI_SIMPLE_NETWORK_RECEIVE_INTERRUPT : 0);
      N->Interrupts = 0;
    }

    if (Buffer && N->TxCompleted) {
      *Buffer        = N->TxBuffer;
      N->TxBuffer    = NULL;
      N->TxCompleted = FALSE;
    }
  }

  gBS->RestoreTPL (Old);
  return Status;
}

STATIC EFI_STATUS EFIAPI
Transmit (
  EFI_SIMPLE_NETWORK_PROTOCOL  *This,
  UINTN                        Header,
  UINTN                        Bytes,
  VOID                         *Buffer,
  EFI_MAC_ADDRESS              *Source,
  EFI_MAC_ADDRESS              *Destination,
  UINT16                       *Protocol
  )
{
  NIC         *N     = From (This);
  EFI_STATUS  Status = State (N);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Buffer || (Bytes < 14) || (Bytes > 1514) ||
      (Header && ((Header != 14) || !Destination || !Protocol)))
  {
    return EFI_INVALID_PARAMETER;
  }

  EFI_TPL  Old = gBS->RaiseTPL (TPL_NOTIFY);

  if (N->TxBuffer) {
    Status = EFI_NOT_READY;
  } else {
    Status = Link (N);
    if (!EFI_ERROR (Status) && !N->Mode.MediaPresent) {
      Status = EFI_NOT_READY;
    }

    if (!EFI_ERROR (Status)) {
      if (Header) {
        UINT8  *Packet = Buffer;
        CopyMem (Packet, Destination->Addr, 6);
        CopyMem (
          Packet + 6,
          Source ? Source->Addr :
          N->Mode.CurrentAddress.Addr,
          6
          );
        Packet[12] = (UINT8)(*Protocol >> 8);
        Packet[13] = (UINT8)*Protocol;
      }

      Status = Rtl8127PacketsTransmit (N->Packets, Buffer, Bytes);
      if (!EFI_ERROR (Status)) {
        N->TxBuffer    = Buffer;
        N->TxCompleted = FALSE;
        N->Stats.TxTotalFrames++;
        N->Stats.TxTotalBytes += Bytes;
        if (Broadcast (Buffer)) {
          N->Stats.TxBroadcastFrames++;
        } else if (*(UINT8 *)Buffer & 1) {
          N->Stats.TxMulticastFrames++;
        } else {
          N->Stats.TxUnicastFrames++;
        }
      } else if (Status != EFI_NOT_READY) {
        N->Faulted = TRUE;
      }
    }
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
  EFI_MAC_ADDRESS              *Source,
  EFI_MAC_ADDRESS              *Destination,
  UINT16                       *Protocol
  )
{
  NIC         *N     = From (This);
  EFI_STATUS  Status = State (N);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Bytes || !Buffer) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_TPL  Old = gBS->RaiseTPL (TPL_NOTIFY);

  Status = PollRx (N);
  if (!EFI_ERROR (Status)) {
    if (*Bytes < N->RxBytes) {
      *Bytes = N->RxBytes;
      Status = EFI_BUFFER_TOO_SMALL;
    } else {
      *Bytes = N->RxBytes;
      CopyMem (Buffer, N->RxBuffer, N->RxBytes);
      if (Header) {
        *Header = 14;
      }

      if (Source) {
        ZeroMem (Source, sizeof (*Source));
        CopyMem (Source->Addr, N->RxBuffer + 6, 6);
      }

      if (Destination) {
        ZeroMem (Destination, sizeof (*Destination));
        CopyMem (Destination->Addr, N->RxBuffer, 6);
      }

      if (Protocol) {
        *Protocol = ((UINT16)N->RxBuffer[12] << 8) | N->RxBuffer[13];
      }

      N->RxBytes = 0;
    }
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

  if (!EFI_ERROR (State (N)) && !EFI_ERROR (PollRx (N))) {
    gBS->SignalEvent (Event);
  }
}

EFI_STATUS
Rtl8127SnpCreate (
  EFI_PCI_IO_PROTOCOL          *Pci,
  CONST UINT8                  Mac[6],
  EFI_SIMPLE_NETWORK_PROTOCOL  **Snp
  )
{
  if (!Pci || !Mac || !Snp || !ValidMac (Mac)) {
    return EFI_INVALID_PARAMETER;
  }

  *Snp = NULL;
  NIC  *N = AllocateZeroPool (sizeof (*N));

  if (!N) {
    return EFI_OUT_OF_RESOURCES;
  }

  N->Signature                 = SIGNATURE;
  N->Pci                       = Pci;
  N->Mode.HwAddressSize        = 6;
  N->Mode.MediaHeaderSize      = 14;
  N->Mode.MaxPacketSize        = 1500;
  N->Mode.IfType               = 1;
  N->Mode.ReceiveFilterMask    = FILTERS;
  N->Mode.ReceiveFilterSetting = EFI_SIMPLE_NETWORK_RECEIVE_UNICAST |
                                 EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST;
  N->Mode.MaxMCastFilterCount   = MAX_MCAST_FILTER_CNT;
  N->Mode.MacAddressChangeable  = TRUE;
  N->Mode.MultipleTxSupported   = FALSE;
  N->Mode.MediaPresentSupported = TRUE;
  CopyMem (N->Mode.PermanentAddress.Addr, Mac, 6);
  CopyMem (N->Mode.CurrentAddress.Addr, Mac, 6);
  SetMem (N->Mode.BroadcastAddress.Addr, 6, 0xff);
  N->Snp = (EFI_SIMPLE_NETWORK_PROTOCOL) {
    EFI_SIMPLE_NETWORK_PROTOCOL_REVISION, Start, Stop, Initialize, Reset,
    Shutdown, ReceiveFilters, StationAddress, Statistics, Mcast, NvData,
    GetStatus, Transmit, Receive, NULL, &N->Mode
  };
  EFI_STATUS  Status = gBS->CreateEvent (
                              EVT_NOTIFY_WAIT,
                              TPL_NOTIFY,
                              WaitPacket,
                              N,
                              &N->Snp.WaitForPacket
                              );

  if (EFI_ERROR (Status)) {
    FreePool (N);
    return Status;
  }

  *Snp = &N->Snp;
  return EFI_SUCCESS;
}

EFI_STATUS
Rtl8127SnpDestroy (
  EFI_SIMPLE_NETWORK_PROTOCOL  **Snp
  )
{
  if (!Snp) {
    return EFI_INVALID_PARAMETER;
  }

  NIC  *N = From (*Snp);

  if (!N) {
    return *Snp ? EFI_INVALID_PARAMETER : EFI_SUCCESS;
  }

  if (N->Ended) {
    return EFI_DEVICE_ERROR;
  }

  EFI_TPL     Old    = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  Status = Rtl8127PacketsClose (&N->Packets);

  if (EFI_ERROR (Status)) {
    gBS->RestoreTPL (Old);
    return Status;
  }

  Status = gBS->CloseEvent (N->Snp.WaitForPacket);
  if (EFI_ERROR (Status)) {
    gBS->RestoreTPL (Old);
    return Status;
  }

  N->Signature = 0;
  FreePool (N);
  *Snp = NULL;
  gBS->RestoreTPL (Old);
  return EFI_SUCCESS;
}

EFI_STATUS
Rtl8127SnpExit (
  EFI_SIMPLE_NETWORK_PROTOCOL  *Snp
  )
{
  NIC  *N = From (Snp);

  if (!N) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_TPL  Old = gBS->RaiseTPL (TPL_NOTIFY);

  N->Ended = TRUE;
  EFI_STATUS  Status = Rtl8127PacketsClose (&N->Packets);

  gBS->RestoreTPL (Old);
  return Status;
}
