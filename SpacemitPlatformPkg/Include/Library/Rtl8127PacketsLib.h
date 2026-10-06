/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef RTL8127_PACKETS_LIB_H
#define RTL8127_PACKETS_LIB_H
#include <Uefi.h>
#include <Protocol/PciIo.h>

typedef struct RTL8127_PACKETS RTL8127_PACKETS;

/* Caller owns the PCI function. A non-NULL result remains owned on an
   error: Close must succeed before freeing the instance or its code. */
EFI_STATUS
Rtl8127PacketsOpen (
  EFI_PCI_IO_PROTOCOL  *Pci,
  CONST UINT8          Mac[6],
  RTL8127_PACKETS      **Packets
  );

EFI_STATUS
Rtl8127PacketsTransmit (
  RTL8127_PACKETS  *Packets,
  CONST VOID       *Buffer,
  UINTN            Bytes
  );

EFI_STATUS
Rtl8127PacketsTxDone (
  RTL8127_PACKETS  *Packets
  );

EFI_STATUS
Rtl8127PacketsReceive (
  RTL8127_PACKETS  *Packets,
  VOID             *Buffer,
  UINTN            *Bytes
  );

EFI_STATUS
Rtl8127PacketsFilters (
  RTL8127_PACKETS  *Packets,
  BOOLEAN          Unicast,
  BOOLEAN          Broadcast,
  BOOLEAN          Multicast,
  BOOLEAN          Promiscuous
  );

EFI_STATUS
Rtl8127PacketsAddress (
  RTL8127_PACKETS  *Packets,
  CONST UINT8      Mac[6]
  );

EFI_STATUS
Rtl8127PacketsLink (
  RTL8127_PACKETS  *Packets,
  BOOLEAN          *Present
  );

EFI_STATUS
Rtl8127PacketsClose (
  RTL8127_PACKETS  **Packets
  );

#endif
