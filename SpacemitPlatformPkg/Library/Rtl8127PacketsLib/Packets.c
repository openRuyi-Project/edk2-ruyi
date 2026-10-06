/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Internal.h"
#include <IndustryStandard/Acpi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

#define RX_COUNT    16
#define SLOT_BYTES  2048
#define OWN         BIT31
#define END_RING    BIT30
#define FIRST       BIT29
#define LAST        BIT28
#define RX_ERROR    BIT21
#define ACTIVE      (BIT3 | BIT2)

typedef struct {
  volatile UINT32    Flags;
  UINT32             Options;
  UINT64             Address;
} DESCRIPTOR;
STATIC_ASSERT (sizeof (DESCRIPTOR) == 16, "RTL descriptor size");

typedef struct {
  VOID                    *Host;
  VOID                    *Mapping;
  EFI_PHYSICAL_ADDRESS    Device;
  UINTN                   Pages;
} COMMON;

struct RTL8127_PACKETS {
  EFI_PCI_IO_PROTOCOL    *Pci;
  UINT64                 OriginalAttributes;
  COMMON                 Rings;
  COMMON                 Data;
  DESCRIPTOR             *Tx;
  DESCRIPTOR             *Rx;
  UINTN                  RxIndex;
  UINT8                  OriginalMac[6];
  BOOLEAN                Registers;
  BOOLEAN                MacSaved;
  BOOLEAN                MayDma;
  BOOLEAN                Ready;
  BOOLEAN                TxPending;
  BOOLEAN                DescriptorsInstalled;
};

STATIC EFI_STATUS
Allocate (
  EFI_PCI_IO_PROTOCOL  *Pci,
  COMMON               *Buffer,
  UINTN                Pages
  )
{
  Buffer->Pages = Pages;
  EFI_STATUS  Status = Pci->AllocateBuffer (
                              Pci,
                              AllocateAnyPages,
                              EfiRuntimeServicesData,
                              Pages,
                              &Buffer->Host,
                              0
                              );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  UINTN  Bytes = EFI_PAGES_TO_SIZE (Pages);

  Status = Pci->Map (
                  Pci,
                  EfiPciIoOperationBusMasterCommonBuffer,
                  Buffer->Host,
                  &Bytes,
                  &Buffer->Device,
                  &Buffer->Mapping
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Buffer->Mapping || (Bytes != EFI_PAGES_TO_SIZE (Pages)) ||
      ((UINTN)Buffer->Host & EFI_PAGE_MASK) ||
      (Buffer->Device & EFI_PAGE_MASK) ||
      (Buffer->Device > MAX_UINT64 - Bytes))
  {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (Buffer->Host, Bytes);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Release (
  EFI_PCI_IO_PROTOCOL  *Pci,
  COMMON               *Buffer
  )
{
  EFI_STATUS  Status;

  if (Buffer->Mapping) {
    Status = Pci->Unmap (Pci, Buffer->Mapping);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Buffer->Mapping = NULL;
  }

  if (Buffer->Host) {
    Status = Pci->FreeBuffer (Pci, Buffer->Pages, Buffer->Host);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Buffer->Host = NULL;
  }

  return EFI_SUCCESS;
}

STATIC VOID
Rearm (
  RTL8127_PACKETS  *P
  )
{
  DESCRIPTOR  *D = &P->Rx[P->RxIndex];

  D->Address = P->Data.Device + P->RxIndex * SLOT_BYTES;
  D->Options = 0;
  MemoryFence ();
  D->Flags = OWN | SLOT_BYTES |
             (P->RxIndex + 1 == RX_COUNT ? END_RING : 0);
  MemoryFence ();
  P->RxIndex = (P->RxIndex + 1) % RX_COUNT;
}

EFI_STATUS
Rtl8127PacketsOpen (
  EFI_PCI_IO_PROTOCOL  *Pci,
  CONST UINT8          Mac[6],
  RTL8127_PACKETS      **Packets
  )
{
  EFI_STATUS  Status;
  UINT32      Id, Revision;
  UINT8       Command;
  UINT8       Any        = 0;
  VOID        *Resources = NULL;

  if (!Pci || !Mac || !Packets) {
    return EFI_INVALID_PARAMETER;
  }

  *Packets = NULL;
  for (UINTN I = 0; I < 6; I++) {
    Any |= Mac[I];
  }

  if (!Any || (Mac[0] & 1)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = Pci->Pci.Read (Pci, EfiPciIoWidthUint32, 0, 1, &Id);
  if (EFI_ERROR (Status) || (Id != 0x812710ec)) {
    return EFI_ERROR (Status) ? Status : EFI_UNSUPPORTED;
  }

  Status = Pci->GetBarAttributes (Pci, 2, NULL, &Resources);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Bar  = Resources;
  BOOLEAN                            Valid = Bar && Bar->Desc == ACPI_ADDRESS_SPACE_DESCRIPTOR &&
                                             Bar->Len >= sizeof (*Bar) - 3 &&
                                             Bar->ResType == ACPI_ADDRESS_SPACE_TYPE_MEM &&
                                             Bar->AddrLen >= 0x604a;

  if (Resources) {
    FreePool (Resources);
  }

  if (!Valid) {
    return EFI_UNSUPPORTED;
  }

  RTL8127_PACKETS  *P = AllocateZeroPool (sizeof (*P));

  if (!P) {
    return EFI_OUT_OF_RESOURCES;
  }

  P->Pci = Pci;
  Status = Pci->Attributes (
                  Pci,
                  EfiPciIoAttributeOperationGet,
                  0,
                  &P->OriginalAttributes
                  );
  if (EFI_ERROR (Status) ||
      (P->OriginalAttributes & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))
  {
    FreePool (P);
    return EFI_ERROR (Status) ? Status : EFI_ACCESS_DENIED;
  }

  *Packets = P;
  Status   = Pci->Attributes (
                    Pci,
                    EfiPciIoAttributeOperationEnable,
                    EFI_PCI_IO_ATTRIBUTE_MEMORY |
                    EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE,
                    NULL
                    );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  P->Registers = TRUE;
  Status       = R27Read (Pci, EfiPciIoWidthUint32, 0x40, &Revision);
  if (!EFI_ERROR (Status)) {
    Status = R27Read (Pci, EfiPciIoWidthUint8, 0x37, &Command);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (((Revision & 0x7cf00000) != 0x6c900000) ||
      (Command & (ACTIVE | BIT4)))
  {
    return EFI_UNSUPPORTED;
  }

  Status = Pci->Mem.Read (
                      Pci,
                      EfiPciIoWidthUint8,
                      2,
                      0,
                      6,
                      P->OriginalMac
                      );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  P->MacSaved = TRUE;
  Status      = Allocate (Pci, &P->Rings, 1);
  if (!EFI_ERROR (Status)) {
    Status = Allocate (
               Pci,
               &P->Data,
               EFI_SIZE_TO_PAGES ((RX_COUNT + 1) * SLOT_BYTES)
               );
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  P->Tx          = P->Rings.Host;
  P->Rx          = (DESCRIPTOR *)((UINT8 *)P->Rings.Host + 256);
  P->Tx->Address = P->Data.Device + RX_COUNT * SLOT_BYTES;
  P->Tx->Flags   = END_RING;
  for (UINTN I = 0; I < RX_COUNT; I++) {
    P->Rx[I].Address = P->Data.Device + I * SLOT_BYTES;
    P->RxIndex       = I;
    Rearm (P);
  }

  P->RxIndex = 0;
  Status     = R27PrepareMac (Pci, Mac);
  if (!EFI_ERROR (Status)) {
    Status = R27Write (Pci, EfiPciIoWidthUint32, 0x38, 0);
  }

  STATIC CONST UINT32  Registers[] = { 0x24, 0x20, 0xe8, 0xe4 };
  UINT64               RxDevice    = P->Rings.Device + 256;
  UINT32               Values[]    = {
    (UINT32)(P->Rings.Device >> 32), (UINT32)P->Rings.Device,
    (UINT32)(RxDevice >> 32),        (UINT32)RxDevice
  };

  for (UINTN I = 0; !EFI_ERROR (Status) && I < ARRAY_SIZE (Registers); I++) {
    P->DescriptorsInstalled = TRUE;
    Status                  = R27Write (Pci, EfiPciIoWidthUint32, Registers[I], Values[I]);
  }

  if (!EFI_ERROR (Status)) {
    Status = R27Write (Pci, EfiPciIoWidthUint16, 0xda, SLOT_BYTES);
  }

  if (!EFI_ERROR (Status)) {
    /* Version-80 defaults, no VLAN stripping or checksum offload.
       Accept the current address and broadcast frames. */
    Status = R27Write (
               Pci,
               EfiPciIoWidthUint32,
               0x44,
               (8U << 27) | (7U << 8) | BIT11 | BIT3 | BIT1
               );
  }

  if (!EFI_ERROR (Status)) {
    Status = R27Write (
               Pci,
               EfiPciIoWidthUint32,
               0x40,
               (3U << 24) | (7U << 8) | BIT7
               );
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  MemoryFence ();
  P->MayDma = TRUE;
  Status    = Pci->Attributes (
                     Pci,
                     EfiPciIoAttributeOperationEnable,
                     EFI_PCI_IO_ATTRIBUTE_BUS_MASTER,
                     NULL
                     );
  if (!EFI_ERROR (Status)) {
    Status = R27Write (Pci, EfiPciIoWidthUint8, 0x37, ACTIVE);
  }

  if (!EFI_ERROR (Status)) {
    P->Ready = TRUE;
  }

  return Status;
}

EFI_STATUS
Rtl8127PacketsTransmit (
  RTL8127_PACKETS  *P,
  CONST VOID       *Buffer,
  UINTN            Bytes
  )
{
  if (!P || !Buffer || (Bytes < 14) || (Bytes > 1518)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!P->Ready || P->TxPending) {
    return EFI_NOT_READY;
  }

  UINT8  *Destination = (UINT8 *)P->Data.Host + RX_COUNT * SLOT_BYTES;
  UINTN  WireBytes    = MAX (Bytes, 60);

  ZeroMem (Destination, WireBytes);
  CopyMem (Destination, Buffer, Bytes);
  P->Tx->Options = 0;
  MemoryFence ();
  P->TxPending = TRUE;
  P->Tx->Flags = OWN | END_RING | FIRST | LAST | (UINT32)WireBytes;
  MemoryFence ();
  EFI_STATUS  Status = R27Write (P->Pci, EfiPciIoWidthUint16, 0x90, 1);

  if (EFI_ERROR (Status)) {
    P->Ready = FALSE;
  }

  return Status;
}

EFI_STATUS
Rtl8127PacketsTxDone (
  RTL8127_PACKETS  *P
  )
{
  if (!P || !P->Ready || !P->TxPending) {
    return EFI_NOT_READY;
  }

  MemoryFence ();
  if (P->Tx->Flags & OWN) {
    return EFI_NOT_READY;
  }

  MemoryFence ();
  P->TxPending = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS
Rtl8127PacketsReceive (
  RTL8127_PACKETS  *P,
  VOID             *Buffer,
  UINTN            *Bytes
  )
{
  if (!P || !Bytes) {
    return EFI_INVALID_PARAMETER;
  }

  if (!P->Ready) {
    return EFI_NOT_READY;
  }

  // Even with IMR masked, service W1C status like the rge interrupt path.
  // Descriptor starvation stops RX until its pending status is acknowledged.
  UINT32      Pending;
  EFI_STATUS  Status = R27Read (
                         P->Pci,
                         EfiPciIoWidthUint32,
                         0x3c,
                         &Pending
                         );

  if (!EFI_ERROR (Status) && (Pending == MAX_UINT32)) {
    Status = EFI_DEVICE_ERROR;
  }

  if (!EFI_ERROR (Status) && Pending) {
    Status = R27Write (P->Pci, EfiPciIoWidthUint32, 0x3c, Pending);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  MemoryFence ();
  UINT32  Flags = P->Rx[P->RxIndex].Flags;

  if (Flags & OWN) {
    return EFI_NOT_READY;
  }

  MemoryFence ();
  UINTN  Length = Flags & 0x3fff;

  if ((Flags & RX_ERROR) || ((Flags & (FIRST | LAST)) != (FIRST | LAST)) ||
      (Length < 18) || (Length > SLOT_BYTES))
  {
    Rearm (P);
    return EFI_DEVICE_ERROR;
  }

  Length -= 4; // Hardware includes the Ethernet FCS.
  if (!Buffer || (*Bytes < Length)) {
    *Bytes = Length;
    return EFI_BUFFER_TOO_SMALL;
  }

  CopyMem (Buffer, (UINT8 *)P->Data.Host + P->RxIndex * SLOT_BYTES, Length);
  *Bytes = Length;
  Rearm (P);
  return EFI_SUCCESS;
}

EFI_STATUS
Rtl8127PacketsFilters (
  RTL8127_PACKETS  *P,
  BOOLEAN          Unicast,
  BOOLEAN          Broadcast,
  BOOLEAN          Multicast,
  BOOLEAN          Promiscuous
  )
{
  if (!P) {
    return EFI_INVALID_PARAMETER;
  }

  if (!P->Ready) {
    return EFI_NOT_READY;
  }

  UINT32      Config;
  EFI_STATUS  Status = R27Read (
                         P->Pci,
                         EfiPciIoWidthUint32,
                         0x44,
                         &Config
                         );
  UINT32  Accept = (Unicast ? BIT1 : 0) |
                   (Broadcast ? BIT3 : 0) |
                   (Multicast ? BIT2 : 0);

  if (Promiscuous) {
    Accept = BIT0 | BIT1 | BIT2 | BIT3;
  }

  if (!EFI_ERROR (Status)) {
    /* Accept all multicast hashes; the SNP layer checks its exact list. */
    Status = R27Write (
               P->Pci,
               EfiPciIoWidthUint32,
               0x08,
               (Multicast || Promiscuous) ? MAX_UINT32 : 0
               );
  }

  if (!EFI_ERROR (Status)) {
    Status = R27Write (
               P->Pci,
               EfiPciIoWidthUint32,
               0x0c,
               (Multicast || Promiscuous) ? MAX_UINT32 : 0
               );
  }

  if (!EFI_ERROR (Status)) {
    Status = R27Write (
               P->Pci,
               EfiPciIoWidthUint32,
               0x44,
               (Config & ~0x3fU) | Accept
               );
  }

  if (EFI_ERROR (Status)) {
    P->Ready = FALSE;
  }

  return Status;
}

EFI_STATUS
Rtl8127PacketsAddress (
  RTL8127_PACKETS  *P,
  CONST UINT8      Mac[6]
  )
{
  if (!P || !Mac) {
    return EFI_INVALID_PARAMETER;
  }

  if (!P->Ready || P->TxPending) {
    return EFI_NOT_READY;
  }

  UINT8  Any = 0;

  for (UINTN I = 0; I < 6; I++) {
    Any |= Mac[I];
  }

  if (!Any || (Mac[0] & 1)) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_STATUS  Status = R27Write (P->Pci, EfiPciIoWidthUint8, 0x50, 0xc0);

  if (!EFI_ERROR (Status)) {
    Status = P->Pci->Mem.Write (
                           P->Pci,
                           EfiPciIoWidthUint8,
                           2,
                           0,
                           6,
                           (VOID *)Mac
                           );
  }

  EFI_STATUS  Lock = R27Write (P->Pci, EfiPciIoWidthUint8, 0x50, 0);

  if (!EFI_ERROR (Status)) {
    Status = Lock;
  }

  UINT8  Actual[6];

  if (!EFI_ERROR (Status)) {
    Status = P->Pci->Mem.Read (
                           P->Pci,
                           EfiPciIoWidthUint8,
                           2,
                           0,
                           6,
                           Actual
                           );
    if (!EFI_ERROR (Status) && CompareMem (Actual, Mac, 6)) {
      Status = EFI_DEVICE_ERROR;
    }
  }

  if (EFI_ERROR (Status)) {
    P->Ready = FALSE;
  }

  return Status;
}

EFI_STATUS
Rtl8127PacketsLink (
  RTL8127_PACKETS  *P,
  BOOLEAN          *Present
  )
{
  if (!P || !Present) {
    return EFI_INVALID_PARAMETER;
  }

  *Present = FALSE;
  if (!P->Ready) {
    return EFI_NOT_READY;
  }

  UINT16      Phy;
  EFI_STATUS  Status = R27Read (P->Pci, EfiPciIoWidthUint16, 0x6c, &Phy);

  if (!EFI_ERROR (Status)) {
    *Present = (Phy & BIT1) != 0;
  }

  return Status;
}

EFI_STATUS
Rtl8127PacketsClose (
  RTL8127_PACKETS  **Packets
  )
{
  if (!Packets) {
    return EFI_INVALID_PARAMETER;
  }

  RTL8127_PACKETS  *P = *Packets;

  if (!P) {
    return EFI_SUCCESS;
  }

  EFI_STATUS  Status;

  P->Ready = FALSE;
  if (P->MayDma) {
    Status = R27Drain (P->Pci);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = P->Pci->Attributes (
                       P->Pci,
                       EfiPciIoAttributeOperationDisable,
                       EFI_PCI_IO_ATTRIBUTE_BUS_MASTER,
                       NULL
                       );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    UINT64  Attributes;

    Status = P->Pci->Attributes (
                       P->Pci,
                       EfiPciIoAttributeOperationGet,
                       0,
                       &Attributes
                       );
    if (EFI_ERROR (Status) ||
        (Attributes & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))
    {
      return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
    }

    Status = P->Pci->Flush (P->Pci);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    P->MayDma = FALSE;
  }

  if (P->DescriptorsInstalled) {
    STATIC CONST UINT32  Registers[] = { 0x24, 0x20, 0xe8, 0xe4 };

    for (UINTN I = 0; I < ARRAY_SIZE (Registers); I++) {
      Status = R27Write (P->Pci, EfiPciIoWidthUint32, Registers[I], 0);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }

    P->DescriptorsInstalled = FALSE;
  }

  if (P->Registers && P->MacSaved) {
    Status = R27Write (P->Pci, EfiPciIoWidthUint8, 0x50, 0xc0);
    if (!EFI_ERROR (Status)) {
      Status = P->Pci->Mem.Write (
                             P->Pci,
                             EfiPciIoWidthUint8,
                             2,
                             0,
                             6,
                             P->OriginalMac
                             );
    }

    EFI_STATUS  Lock = R27Write (P->Pci, EfiPciIoWidthUint8, 0x50, 0);

    if (EFI_ERROR (Status) || EFI_ERROR (Lock)) {
      return EFI_ERROR (Status) ? Status : Lock;
    }

    P->MacSaved = FALSE;
  }

  Status = Release (P->Pci, &P->Data);
  if (!EFI_ERROR (Status)) {
    Status = Release (P->Pci, &P->Rings);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = P->Pci->Attributes (
                     P->Pci,
                     EfiPciIoAttributeOperationSet,
                     P->OriginalAttributes,
                     NULL
                     );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  FreePool (P);
  *Packets = NULL;
  return EFI_SUCCESS;
}
