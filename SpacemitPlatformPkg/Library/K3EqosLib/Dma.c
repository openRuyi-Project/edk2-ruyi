/** SPDX-License-Identifier: BSD-2-Clause-Patent */

/* DWMAC 5.40a descriptor/register reference: BSD SpacemiT DwEqosDxeUtil.
 * Copyright (c) 2025, Spacemit Corporation. All rights reserved.
 * Buffer/cache lifetime is provided by the independent platform DmaLib.
 */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DmaLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/K3EqosDmaLib.h>
#include <Library/TimerLib.h>

#define OWN                  BIT31
#define FIRST                BIT29
#define LAST                 BIT28
#define ERROR                BIT15
#define TX_OFFSET            0U
#define RX_OFFSET            (K3_EQOS_TX * sizeof (K3_EQOS_DESC))
#define TX_BYTES             ALIGN_VALUE ((K3_EQOS_TX + K3_EQOS_RX) * sizeof (K3_EQOS_DESC), 64)
#define RX_BYTES             (TX_BYTES + K3_EQOS_TX * K3_EQOS_FRAME_BYTES)
#define TOTAL_BYTES          (RX_BYTES + K3_EQOS_RX * K3_EQOS_FRAME_BYTES)
#define RESET_TIMEOUT_US     500000
#define DRAIN_TIMEOUT_US     50000
#define MTL_TXQ_DEBUG        0xd08
#define MTL_TXQ_TRCSTS_MASK  (3U << 1)
#define MTL_TXQ_TRCSTS_READ  BIT1
#define MTL_TXQ_NOT_EMPTY    BIT4
#define MTL_RXQ_DEBUG        0xd38
#define MTL_RXQ_BUSY_MASK    ((0x777fU << 16) | (3U << 4))

STATIC EFI_PHYSICAL_ADDRESS
Address (
  K3_EQOS_DMA  *D,
  CONST VOID   *P
  )
{
  return D->Bus + ((UINTN)P - (UINTN)D->Allocation);
}

STATIC EFI_STATUS
Wait (
  UINTN   Register,
  UINT32  Mask,
  UINT32  Expected,
  UINTN   TimeoutUs
  )
{
  for (UINTN Elapsed = 0; Elapsed <= TimeoutUs; Elapsed += 10) {
    if ((MmioRead32 (Register) & Mask) == Expected) {
      return EFI_SUCCESS;
    }

    if (Elapsed < TimeoutUs) {
      MicroSecondDelay (10);
    }
  }

  return EFI_TIMEOUT;
}

VOID
K3EqosMacAddress (
  K3_EQOS_PORT  *P,
  CONST UINT8   *Mac
  )
{
  MmioWrite32 (P->Base + 0x300, Mac[4] | ((UINT32)Mac[5] << 8) | BIT31);
  MmioWrite32 (P->Base + 0x304, Mac[0] | ((UINT32)Mac[1] << 8) | ((UINT32)Mac[2] << 16) | ((UINT32)Mac[3] << 24));
}

VOID
K3EqosSpeed (
  K3_EQOS_PORT  *P,
  UINT32        Speed,
  BOOLEAN       Full
  )
{
  UINT32  Set = Full ? BIT13 : 0;

  if (Speed != 1000) {
    Set |= BIT15;
  }

  if (Speed == 100) {
    Set |= BIT14;
  }

  MmioAndThenOr32 (P->Base, ~(BIT13 | BIT14 | BIT15), Set);
}

STATIC VOID
RxReturn (
  K3_EQOS_DMA  *D,
  UINTN        Index
  )
{
  EFI_PHYSICAL_ADDRESS  Buffer = Address (D, D->RxBytes + Index * K3_EQOS_FRAME_BYTES);

  D->Rx[Index].Word[0] = (UINT32)Buffer;
  D->Rx[Index].Word[1] = (UINT32)(Buffer >> 32);
  D->Rx[Index].Word[2] = 0;
  MemoryFence ();
  D->Rx[Index].Word[3] = OWN | BIT24;
  MemoryFence ();
  MmioWrite32 (D->Port->Base + 0x1128, (UINT32)Address (D, &D->Rx[Index]));
}

EFI_STATUS
K3EqosDmaStart (
  K3_EQOS_DMA   *D,
  K3_EQOS_PORT  *P,
  CONST UINT8   *Mac
  )
{
  if (!D || !P || !Mac || !P->PhyConfigured || P->Faulted || D->Allocation || D->Running) {
    return EFI_NOT_READY;
  }

  if ((P->AddressBits != 40) && (P->AddressBits != 48)) {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (D, sizeof (*D));
  D->Port  = P;
  D->Pages = EFI_SIZE_TO_PAGES (TOTAL_BYTES);
  EFI_STATUS  Status = DmaAllocateBuffer (EfiBootServicesData, D->Pages, &D->Allocation);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  UINTN  Bytes = EFI_PAGES_TO_SIZE (D->Pages);

  Status = DmaMap (MapOperationBusMasterCommonBuffer, D->Allocation, &Bytes, &D->Bus, &D->Mapping);
  if (EFI_ERROR (Status)) {
    K3EqosDmaFree (D);
    return Status;
  }

  if ((Bytes != EFI_PAGES_TO_SIZE (D->Pages)) || (D->Bus > (1ULL << P->AddressBits) - Bytes) ||
      (D->Bus & 63) || ((D->Bus >> 32) != ((D->Bus + TX_BYTES - 1) >> 32)))
  {
    K3EqosDmaFree (D);
    return EFI_UNSUPPORTED;
  }

  ZeroMem (D->Allocation, Bytes);
  D->Tx      = (VOID *)((UINT8 *)D->Allocation + TX_OFFSET);
  D->Rx      = (VOID *)((UINT8 *)D->Allocation + RX_OFFSET);
  D->TxBytes = (UINT8 *)D->Allocation + TX_BYTES;
  D->RxBytes = (UINT8 *)D->Allocation + RX_BYTES;
  // No DMA start bit or device pointer is written until buffers are usable.
  MmioOr32 (P->Base + 0x1000, BIT0);
  // RTL8211F link-down power saving can pause RXC for hundreds of ms.
  Status = Wait (P->Base + 0x1000, BIT0, 0, RESET_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    D->Faulted = TRUE;
    return Status;
  }

  UINT32  Features = MmioRead32 (P->Base + 0x120);
  UINT32  TxFifo = 128U << ((Features >> 6) & 0x1f), RxFifo = 128U << (Features & 0x1f);

  if ((TxFifo < 256) || (TxFifo > 32768) || (RxFifo < 256) || (RxFifo > 32768)) {
    K3EqosDmaFree (D);
    return EFI_UNSUPPORTED;
  }

  MmioAndThenOr32 (
    P->Base + 0xd00,
    ~((0x1ffU << 16) | (3U << 2)),
    ((TxFifo / 256 - 1) << 16) | (2U << 2) | BIT1
    );
  MmioAndThenOr32 (P->Base + 0xd30, ~(0x3ffU << 20), ((RxFifo / 256 - 1) << 20) | BIT5);
  MmioAndThenOr32 (P->Base + 0xa0, ~3U, 2);
  MmioAndThenOr32 (P->Base + 0x1100, ~(7U << 18), BIT16); // Contiguous 16-byte UC descriptors, DSL=0.
  MmioAndThenOr32 (P->Base + 0x1104, ~(0x3fU << 16), (8U << 16) | BIT4);
  MmioAndThenOr32 (
    P->Base + 0x1108,
    ~((0x3fU << 16) | (0x3fffU << 1)),
    (8U << 16) | (K3_EQOS_FRAME_BYTES << 1)
    );
  MmioWrite32 (P->Base + 0x1004, (0xfU << 24) | (0xfU << 16) | BIT11 | BIT3 | BIT2 | BIT1);
  MmioWrite32 (P->Base + 0x1134, 0); // Firmware polls; do not enable an unowned IRQ route.
  MmioOr32 (P->Base, BIT21 | BIT20);
  K3EqosMacAddress (P, Mac);
  EFI_PHYSICAL_ADDRESS  Tx = Address (D, D->Tx), Rx = Address (D, D->Rx);

  MmioWrite32 (P->Base + 0x1110, (UINT32)(Tx >> 32));
  MmioWrite32 (P->Base + 0x1114, (UINT32)Tx);
  MmioWrite32 (P->Base + 0x1118, (UINT32)(Rx >> 32));
  MmioWrite32 (P->Base + 0x111c, (UINT32)Rx);
  MmioWrite32 (P->Base + 0x112c, K3_EQOS_TX - 1);
  MmioWrite32 (P->Base + 0x1130, K3_EQOS_RX - 1);
  for (UINTN I = 0; I < K3_EQOS_RX; I++) {
    RxReturn (D, I);
  }

  MemoryFence ();
  MmioOr32 (P->Base + 0x1104, BIT0);
  MmioOr32 (P->Base + 0x1108, BIT0);
  MmioOr32 (P->Base, BIT0 | BIT1);
  D->Running = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
K3EqosDmaStop (
  K3_EQOS_DMA  *D
  )
{
  if (!D || !D->Port) {
    return EFI_INVALID_PARAMETER;
  }

  K3_EQOS_PORT  *P = D->Port;

  if (!D->Running && !D->Faulted) {
    return EFI_SUCCESS;
  }

  MmioAnd32 (P->Base + 0x1104, ~BIT0);
  EFI_STATUS  Status = EFI_TIMEOUT;

  for (UINTN Us = 0; Us <= DRAIN_TIMEOUT_US; Us += 10) {
    UINT32  Debug = MmioRead32 (P->Base + MTL_TXQ_DEBUG);

    // TRCSTS=1 reads packet data. Waiting/writing TX status is also
    // quiescent once TXQSTS reports an empty queue.
    if (((Debug & MTL_TXQ_TRCSTS_MASK) != MTL_TXQ_TRCSTS_READ) &&
        !(Debug & MTL_TXQ_NOT_EMPTY))
    {
      Status = EFI_SUCCESS;
      break;
    }

    if (Us < DRAIN_TIMEOUT_US) {
      MicroSecondDelay (10);
    }
  }

  MmioAnd32 (P->Base, ~(BIT0 | BIT1));
  if (!EFI_ERROR (Status)) {
    Status = EFI_TIMEOUT;
    for (UINTN Us = 0; Us <= DRAIN_TIMEOUT_US; Us += 10) {
      if (!(MmioRead32 (P->Base + MTL_RXQ_DEBUG) & MTL_RXQ_BUSY_MASK)) {
        Status = EFI_SUCCESS;
        break;
      }

      // With MAC receive off, recycle completed descriptors to drain
      // queued packets even when the SNP consumer has stopped.
      for (UINTN Index = 0; Index < K3_EQOS_RX; Index++) {
        if (!(D->Rx[Index].Word[3] & OWN)) {
          RxReturn (D, Index);
        }
      }

      if (Us < DRAIN_TIMEOUT_US) {
        MicroSecondDelay (10);
      }
    }
  }

  MmioAnd32 (P->Base + 0x1108, ~BIT0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "K3 EQoS: queues did not drain; resetting DMA\n"));
  }

  // Reset also flushes queued packets if graceful draining timed out.
  // Keep buffers quarantined unless the controller confirms completion.
  MmioOr32 (P->Base + 0x1000, BIT0);
  Status = Wait (P->Base + 0x1000, BIT0, 0, RESET_TIMEOUT_US);

  if (EFI_ERROR (Status)) {
    D->Faulted = TRUE;
    return Status;
  }

  D->Running = D->Faulted = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS
K3EqosDmaFree (
  K3_EQOS_DMA  *D
  )
{
  if (!D || D->Running || D->Faulted) {
    return EFI_NOT_READY;
  }

  if (!D->Allocation) {
    return EFI_SUCCESS;
  }

  EFI_STATUS  Status;

  if (D->Mapping) {
    Status = DmaUnmap (D->Mapping);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    D->Mapping = NULL;
  }

  Status = DmaFreeBuffer (D->Pages, D->Allocation);
  if (!EFI_ERROR (Status)) {
    ZeroMem (D, sizeof (*D));
  }

  return Status;
}

EFI_STATUS
K3EqosTransmit (
  K3_EQOS_DMA  *D,
  VOID         *Buffer,
  UINTN        Bytes
  )
{
  if (!D || !Buffer || (Bytes < 14) || (Bytes > K3_EQOS_FRAME_BYTES)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!D->Running || D->Faulted) {
    return EFI_NOT_READY;
  }

  UINTN  Index = D->TxNext;

  if ((D->TxCount == K3_EQOS_TX) || (D->Tx[Index].Word[3] & OWN)) {
    return EFI_NOT_READY;
  }

  UINT8  *Copy = D->TxBytes + Index * K3_EQOS_FRAME_BYTES;

  CopyMem (Copy, Buffer, Bytes);
  EFI_PHYSICAL_ADDRESS  Bus = Address (D, Copy);

  D->Tx[Index].Word[0] = (UINT32)Bus;
  D->Tx[Index].Word[1] = (UINT32)(Bus >> 32);
  D->Tx[Index].Word[2] = (UINT32)Bytes;
  D->Original[Index]   = Buffer;
  MemoryFence ();
  D->Tx[Index].Word[3] = OWN | FIRST | LAST | (UINT32)Bytes;
  D->TxCount++;
  D->TxNext = (Index + 1) % K3_EQOS_TX;
  MemoryFence ();
  MmioWrite32 (D->Port->Base + 0x1120, (UINT32)Address (D, &D->Tx[D->TxNext]));
  return EFI_SUCCESS;
}

EFI_STATUS
K3EqosReclaim (
  K3_EQOS_DMA  *D,
  VOID         **Buffer
  )
{
  if (!D || !Buffer) {
    return EFI_INVALID_PARAMETER;
  }

  *Buffer = NULL;
  if (!D->Running || D->Faulted) {
    return EFI_NOT_READY;
  }

  if (!D->TxCount || (D->Tx[D->TxHead].Word[3] & OWN)) {
    return EFI_SUCCESS;
  }

  MemoryFence ();
  UINT32  Status = D->Tx[D->TxHead].Word[3];

  *Buffer                = D->Original[D->TxHead];
  D->Original[D->TxHead] = NULL;
  D->TxCount--;
  D->TxHead = (D->TxHead + 1) % K3_EQOS_TX;
  if (Status & ERROR) {
    D->TxError++;
    return EFI_DEVICE_ERROR;
  }

  D->TxGood++;
  return EFI_SUCCESS;
}

BOOLEAN
K3EqosPacketReady (
  K3_EQOS_DMA  *D
  )
{
  return D && D->Running && !D->Faulted && !(D->Rx[D->RxHead].Word[3] & OWN);
}

EFI_STATUS
K3EqosReceive (
  K3_EQOS_DMA  *D,
  VOID         *Buffer,
  UINTN        *Bytes
  )
{
  if (!D || !Bytes) {
    return EFI_INVALID_PARAMETER;
  }

  if (!K3EqosPacketReady (D)) {
    return EFI_NOT_READY;
  }

  MemoryFence ();
  UINT32  Status = D->Rx[D->RxHead].Word[3];
  UINTN   Length = Status & 0x7fff;

  if (((Status & (FIRST | LAST | ERROR | BIT30)) != (FIRST | LAST)) || (Length < 14) ||
      (Length > K3_EQOS_FRAME_BYTES))
  {
    D->RxError++;
    RxReturn (D, D->RxHead);
    D->RxHead = (D->RxHead + 1) % K3_EQOS_RX;
    return EFI_DEVICE_ERROR;
  }

  UINTN  Capacity = *Bytes;

  *Bytes = Length;

  if (Capacity < Length) {
    return EFI_BUFFER_TOO_SMALL;
  }

  if (!Buffer) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, D->RxBytes + D->RxHead * K3_EQOS_FRAME_BYTES, Length);
  D->RxGood++;
  RxReturn (D, D->RxHead);
  D->RxHead = (D->RxHead + 1) % K3_EQOS_RX;
  return EFI_SUCCESS;
}
