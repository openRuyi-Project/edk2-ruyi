/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef RTL812X_PACKETS_LIB_H
#define RTL812X_PACKETS_LIB_H
#include <Uefi.h>
#include <Protocol/PciIo.h>

typedef struct RTL812X_PACKETS RTL812X_PACKETS;

/* Caller owns the PCI function. A non-NULL result remains owned on an
   error: Close must succeed before freeing the instance or its code. */
EFI_STATUS
Rtl812xPacketsOpen (
  EFI_PCI_IO_PROTOCOL  *Pci,
  CONST UINT8          Mac[6],
  RTL812X_PACKETS      **Packets
  );

EFI_STATUS
Rtl812xPacketsTransmit (
  RTL812X_PACKETS  *Packets,
  CONST VOID       *Buffer,
  UINTN            Bytes
  );

EFI_STATUS
Rtl812xPacketsTxDone (
  RTL812X_PACKETS  *Packets
  );

EFI_STATUS
Rtl812xPacketsReceive (
  RTL812X_PACKETS  *Packets,
  VOID             *Buffer,
  UINTN            *Bytes
  );

EFI_STATUS
Rtl812xPacketsFilters (
  RTL812X_PACKETS  *Packets,
  BOOLEAN          Unicast,
  BOOLEAN          Broadcast,
  BOOLEAN          Multicast,
  BOOLEAN          Promiscuous
  );

EFI_STATUS
Rtl812xPacketsAddress (
  RTL812X_PACKETS  *Packets,
  CONST UINT8      Mac[6]
  );

EFI_STATUS
Rtl812xPacketsLink (
  RTL812X_PACKETS  *Packets,
  BOOLEAN          *Present
  );

EFI_STATUS
Rtl812xPacketsClose (
  RTL812X_PACKETS  **Packets
  );

#endif
