/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Cadence GEM setup follows U-Boot drivers/net/macb.c.
 * Copyright (C) 2005-2006 Atmel Corporation
 * FU740 uses coherent DMA with 32-bit buffer addresses.
 */
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "Gem.h"

STATIC EFI_STATUS
Poll (
  UINTN   Address,
  UINT32  Mask,
  UINT32  Value,
  UINTN   Us
  )
{
  for (UINTN Time = 0; Time < Us; Time += 10) {
    if ((MmioRead32 (Address) & Mask) == Value) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
Map (
  UINTN   Base,
  UINT64  Size
  )
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;
  EFI_STATUS                       Status;

  if (!Size || ((Base | Size) & EFI_PAGE_MASK) || (Base > MAX_UINT64 - Size)) {
    return EFI_COMPROMISED_DATA;
  }

  Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Base, Size, EFI_MEMORY_UC);
  if (EFI_ERROR (Status)) {
    Status = gDS->GetMemorySpaceDescriptor (Base, &Desc);
    if (EFI_ERROR (Status) || (Desc.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) ||
        (Desc.BaseAddress > Base) || (Desc.Length < Size) || (Base - Desc.BaseAddress > Desc.Length - Size))
    {
      return EFI_UNSUPPORTED;
    }
  }

  return gDS->SetMemorySpaceAttributes (Base, Size, EFI_MEMORY_UC);
}

BOOLEAN
GemFdtEnabled (
  CONST VOID  *Fdt,
  INT32       Node
  )
{
  INT32        Length;
  CONST CHAR8  *Status = FdtGetProp (Fdt, Node, "status", &Length);

  return !Status || (Length == 5 && !AsciiStrCmp (Status, "okay")) ||
         (Length == 3 && !AsciiStrCmp (Status, "ok"));
}

STATIC UINT64
Cells (
  CONST UINT32  *Data
  )
{
  return LShiftU64 (Fdt32ToCpu (Data[0]), 32) | Fdt32ToCpu (Data[1]);
}

EFI_STATUS
GemParse (
  CONST VOID  *Fdt,
  INT32       Node,
  GEM_PORT    *Port
  )
{
  INT32         Length, Phy;
  CONST UINT32  *Data;
  CONST CHAR8   *Mode;
  EFI_STATUS    Status;

  // This integration supports the FU740 GMII port, not other MACB variants.
  if ((FdtNodeOffsetByCompatible (Fdt, -1, "sifive,fu740-c000") < 0) &&
      (FdtNodeOffsetByCompatible (Fdt, -1, "sifive,fu740-c000-prci") < 0))
  {
    return EFI_UNSUPPORTED;
  }

  Mode = FdtGetProp (Fdt, Node, "phy-mode", &Length);
  if (!Mode || (Length != 5) || AsciiStrCmp (Mode, "gmii")) {
    return EFI_UNSUPPORTED;
  }

  Data = FdtGetProp (Fdt, Node, "reg", &Length);
  if (!Data || (Length != 32)) {
    return EFI_COMPROMISED_DATA;
  }

  Port->Base       = Cells (Data);
  Port->Management = Cells (Data + 4);
  if ((Cells (Data + 2) < 0x1000) || (Cells (Data + 6) < 0x1000)) {
    return EFI_COMPROMISED_DATA;
  }

  Status = Map (Port->Base, Cells (Data + 2));
  if (!EFI_ERROR (Status)) {
    Status = Map (Port->Management, Cells (Data + 6));
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Data = FdtGetProp (Fdt, Node, "phy-handle", &Length);
  if (!Data || (Length != 4)) {
    return EFI_COMPROMISED_DATA;
  }

  Phy = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Data));
  if (Phy < 0) {
    return EFI_COMPROMISED_DATA;
  }

  Data = FdtGetProp (Fdt, Phy, "reg", &Length);
  if (!Data || (Length != 4) || (Fdt32ToCpu (*Data) > 31)) {
    return EFI_COMPROMISED_DATA;
  }

  Port->Phy = Fdt32ToCpu (*Data);
  // FU740 GEMGXL is clocked at 125 MHz. /224 keeps MDC below 2.5 MHz.
  Port->MdcDivider = 7U << 18;
  return EFI_SUCCESS;
}

EFI_STATUS
GemMdio (
  GEM_PORT  *Port,
  UINT32    Register,
  BOOLEAN   Read,
  UINT16    *Value
  )
{
  EFI_STATUS  Status;

  Status = Poll (Port->Base + GEM_NSR, BIT2, BIT2, 10000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (
    Port->Base + GEM_MAN,
    BIT30 | (Read ? BIT29 : BIT28) |
    (Port->Phy << 23) | (Register << 18) | BIT17 | (Read ? 0 : *Value)
    );
  Status = Poll (Port->Base + GEM_NSR, BIT2, BIT2, 10000);
  if (!EFI_ERROR (Status) && Read) {
    *Value = (UINT16)MmioRead32 (Port->Base + GEM_MAN);
  }

  return Status;
}

EFI_STATUS
GemPrepare (
  GEM_PORT  *Port
  )
{
  UINT32      Width = (MmioRead32 (Port->Base + GEM_DESIGN1) >> 25) & 7;
  EFI_STATUS  Status;
  UINT16      High, Low;

  Port->BusWidth = (Width == 4 ? 2U : Width == 2 ? 1U : 0U) << 21;
  MmioWrite32 (Port->Base + GEM_IDR, MAX_UINT32);
  MmioWrite32 (Port->Base + GEM_NCR, GEM_TX_HALT | GEM_MDIO_ENABLE);
  Status = Poll (Port->Base + GEM_TSR, BIT3, 0, 100000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (Port->Base + GEM_NCR, GEM_MDIO_ENABLE | GEM_CLEAR_STATS);
  MmioWrite32 (Port->Base + GEM_NCFGR, Port->MdcDivider | Port->BusWidth | BIT17);
  if (MmioRead32 (Port->Base + GEM_DESIGN1) & BIT9) {
    MmioWrite32 (Port->Base + GEM_USRIO, 0); // GMII
  }

  Status = GemMdio (Port, 2, TRUE, &High);
  if (!EFI_ERROR (Status)) {
    Status = GemMdio (Port, 3, TRUE, &Low);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Port->PhyId = ((UINT32)High << 16) | Low;
  return (Port->PhyId == 0 || Port->PhyId == MAX_UINT32) ? EFI_NOT_FOUND : EFI_SUCCESS;
}

EFI_STATUS
GemLink (
  GEM_PORT  *Port,
  BOOLEAN   *Up,
  BOOLEAN   *Full,
  UINT32    *Speed
  )
{
  UINT64      Now = GetPerformanceCounter ();
  UINT16      Status, Aux;
  EFI_STATUS  Result;

  // GetStatus/Transmit can be called per packet. Poll MDIO at most every 100 ms.
  if (!Port->LastLinkCheck || (GetTimeInNanoSecond (Now - Port->LastLinkCheck) >= 100000000)) {
    Result = GemMdio (Port, 1, TRUE, &Status);
    if (!EFI_ERROR (Result)) {
      Result = GemMdio (Port, 1, TRUE, &Status); // BMSR link is latched low.
    }

    if (EFI_ERROR (Result)) {
      return Result;
    }

    Port->Up = (Status & BIT2) != 0;
    if (Port->Up) {
      Result = GemMdio (Port, 28, TRUE, &Aux); // VSC8541 auxiliary status
      if (EFI_ERROR (Result)) {
        return Result;
      }

      UINT32  Rate = (Aux >> 3) & 3;
      if (Rate == 3) {
        return EFI_DEVICE_ERROR;
      }

      Port->Speed = Rate == 2 ? 1000 : Rate == 1 ? 100 : 10;
      Port->Full  = (Aux & BIT5) != 0;
    }

    Port->LastLinkCheck = Now;
  }

  *Up    = Port->Up;
  *Full  = Port->Full;
  *Speed = Port->Speed;
  return EFI_SUCCESS;
}

VOID
GemSpeed (
  GEM_PORT  *Port,
  UINT32    Speed,
  BOOLEAN   Full
  )
{
  UINT32  Value = (Full ? BIT1 : 0) | (Speed == 1000 ? BIT10 : Speed == 100 ? BIT0 : 0);

  if ((Port->AppliedSpeed == Speed) && (Port->AppliedFull == Full)) {
    return;
  }

  MmioWrite32 (Port->Management, Speed != 1000); // 125 MHz GTX_CLK or PHY TX_CLK
  MmioAndThenOr32 (Port->Base + GEM_NCFGR, ~(BIT0 | BIT1 | BIT10), Value);
  Port->AppliedSpeed = Speed;
  Port->AppliedFull  = Full;
}

VOID
GemMacAddress (
  GEM_PORT     *Port,
  CONST UINT8  *Mac
  )
{
  MmioWrite32 (Port->Base + GEM_MAC_LOW, ReadUnaligned32 ((CONST UINT32 *)Mac));
  MmioWrite32 (Port->Base + GEM_MAC_HIGH, ReadUnaligned16 ((CONST UINT16 *)(Mac + 4)));
}

EFI_STATUS
GemDmaStart (
  GEM_DMA      *Dma,
  GEM_PORT     *Port,
  CONST UINT8  *Mac
  )
{
  EFI_PHYSICAL_ADDRESS  Address = MAX_UINT32;
  EFI_STATUS            Status;
  UINT32                Queues;

  if (Dma->Allocation || Dma->Running || Dma->Faulted) {
    return EFI_DEVICE_ERROR;
  }

  // Hold a full 2 MiB TCP receive window while a firmware console update
  // delays polling. A smaller ring drops bursts before TCP can apply flow control.
  UINTN  DescriptorBytes = ALIGN_VALUE ((GEM_RX_COUNT + GEM_TX_COUNT + 1) * sizeof (GEM_DESCRIPTOR), EFI_PAGE_SIZE);

  Dma->Pages = EFI_SIZE_TO_PAGES (DescriptorBytes + (GEM_RX_COUNT + GEM_TX_COUNT) * GEM_FRAME_BYTES);
  Status     = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, Dma->Pages, &Address);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Dma->Port       = Port;
  Dma->Allocation = (VOID *)(UINTN)Address;
  ZeroMem (Dma->Allocation, EFI_PAGES_TO_SIZE (Dma->Pages));
  Dma->Rx      = Dma->Allocation;
  Dma->Tx      = Dma->Rx + GEM_RX_COUNT;
  Dma->Dummy   = Dma->Tx + GEM_TX_COUNT;
  Dma->RxBytes = (UINT8 *)Dma->Allocation + DescriptorBytes;
  Dma->TxBytes = Dma->RxBytes + GEM_RX_COUNT * GEM_FRAME_BYTES;
  Dma->RxHead  = Dma->TxHead = Dma->TxTail = Dma->TxPending = 0;
  ZeroMem (Dma->Original, sizeof (Dma->Original));
  Dma->RxGood = Dma->RxError = Dma->TxGood = Dma->TxError = Dma->TxRestarts = 0;
  for (UINTN Index = 0; Index < GEM_RX_COUNT; Index++) {
    Dma->Rx[Index].Address = (UINT32)(UINTN)(Dma->RxBytes + Index * GEM_FRAME_BYTES) |
                             (Index == GEM_RX_COUNT - 1 ? GEM_RX_WRAP : 0);
  }

  for (UINTN Index = 0; Index < GEM_TX_COUNT; Index++) {
    Dma->Tx[Index].Address = (UINT32)(UINTN)(Dma->TxBytes + Index * GEM_FRAME_BYTES);
    Dma->Tx[Index].Control = GEM_TX_USED | (Index == GEM_TX_COUNT - 1 ? GEM_TX_WRAP : 0);
  }

  Dma->Dummy->Address = GEM_RX_USED | GEM_RX_WRAP;
  Dma->Dummy->Control = GEM_TX_USED | GEM_TX_WRAP;
  MemoryFence ();
  GemMacAddress (Port, Mac);
  for (UINTN Index = 1; Index < 4; Index++) {
    MmioWrite32 (Port->Base + GEM_MAC_LOW + Index * 8, 0);
    MmioWrite32 (Port->Base + GEM_MAC_HIGH + Index * 8, 0);
  }

  MmioWrite32 (Port->Base + GEM_DMA_CONFIG, (GEM_FRAME_BYTES / 64) << 16 | BIT10 | (3U << 8) | 16);
  MmioWrite32 (Port->Base + GEM_RX_QUEUE, (UINT32)(UINTN)Dma->Rx);
  MmioWrite32 (Port->Base + GEM_TX_QUEUE, (UINT32)(UINTN)Dma->Tx);
  Queues = MmioRead32 (Port->Base + GEM_DESIGN6) & 0xff;
  for (UINTN Index = 1; Index < 8; Index++) {
    if (Queues & (1U << Index)) {
      MmioWrite32 (Port->Base + 0x620 + (Index - 1) * 4, MAX_UINT32);
      MmioWrite32 (Port->Base + 0x440 + (Index - 1) * 4, (UINT32)(UINTN)Dma->Dummy);
      MmioWrite32 (Port->Base + 0x480 + (Index - 1) * 4, (UINT32)(UINTN)Dma->Dummy);
    }
  }

  MmioWrite32 (Port->Base + GEM_TSR, MAX_UINT32);
  MmioWrite32 (Port->Base + GEM_RSR, MAX_UINT32);
  MmioWrite32 (Port->Base + GEM_NCR, GEM_MDIO_ENABLE | GEM_TX_ENABLE | GEM_RX_ENABLE);
  Dma->Running        = TRUE;
  Port->LastLinkCheck = 0;
  return EFI_SUCCESS;
}

EFI_STATUS
GemDmaStop (
  GEM_DMA  *Dma
  )
{
  EFI_STATUS  Status;

  if (!Dma->Allocation) {
    return EFI_SUCCESS;
  }

  MmioOr32 (Dma->Port->Base + GEM_NCR, GEM_TX_HALT);
  Status = Poll (Dma->Port->Base + GEM_TSR, BIT3, 0, 100000);
  if (EFI_ERROR (Status)) {
    Dma->Faulted = TRUE;
    return Status;
  }

  MmioWrite32 (Dma->Port->Base + GEM_NCR, GEM_MDIO_ENABLE);
  MmioRead32 (Dma->Port->Base + GEM_NCR);
  MemoryFence ();
  Dma->Running = Dma->Faulted = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS
GemDmaFree (
  GEM_DMA  *Dma
  )
{
  EFI_STATUS  Status;

  if (Dma->Running || Dma->Faulted) {
    return EFI_DEVICE_ERROR;
  }

  if (!Dma->Allocation) {
    return EFI_SUCCESS;
  }

  Status = gBS->FreePages ((UINTN)Dma->Allocation, Dma->Pages);
  if (!EFI_ERROR (Status)) {
    ZeroMem (Dma, sizeof (*Dma));
  }

  return Status;
}

EFI_STATUS
GemTransmit (
  GEM_DMA  *Dma,
  VOID     *Buffer,
  UINTN    Bytes
  )
{
  UINTN  Index = Dma->TxHead;

  if (Dma->TxPending == GEM_TX_COUNT - 1) {
    return EFI_NOT_READY;
  }

  UINTN  Next = (Index + 1) % GEM_TX_COUNT;

  // Keep an explicit unused descriptor after the producer, as macb_tx_map()
  // does. Publish that stop marker before handing the frame to DMA.
  Dma->Tx[Next].Control = GEM_TX_USED | (Next == GEM_TX_COUNT - 1 ? GEM_TX_WRAP : 0);
  CopyMem (Dma->TxBytes + Index * GEM_FRAME_BYTES, Buffer, Bytes);
  Dma->Original[Index] = Buffer;
  MemoryFence ();
  Dma->Tx[Index].Control = (UINT32)Bytes | GEM_TX_LAST | (Index == GEM_TX_COUNT - 1 ? GEM_TX_WRAP : 0);
  MemoryFence ();
  Dma->TxHead = (Index + 1) % GEM_TX_COUNT;
  Dma->TxPending++;
  MmioOr32 (Dma->Port->Base + GEM_NCR, GEM_TX_START);
  return EFI_SUCCESS;
}

VOID
GemRestartTx (
  GEM_DMA  *Dma,
  UINT32   Status
  )
{
  if (!(Status & BIT0) || !Dma->TxPending) {
    return;
  }

  // GEM can prefetch a USED descriptor while software is extending the queue.
  // TSTART during an active transfer does not prevent that later stop. Follow
  // Linux macb_tx_restart(): compare the hardware cursor with the producer and
  // restart if it stopped before the newly queued frames.
  MemoryFence ();
  UINT32  Cursor = MmioRead32 (Dma->Port->Base + GEM_TX_QUEUE) & ~3U;
  UINT32  Base   = (UINT32)(UINTN)Dma->Tx;
  UINT32  Head   = Base + (UINT32)Dma->TxHead * sizeof (*Dma->Tx);

  if ((Cursor >= Base) && (Cursor < Base + GEM_TX_COUNT * sizeof (*Dma->Tx)) &&
      (Cursor != Head))
  {
    Dma->TxRestarts++;
    MmioOr32 (Dma->Port->Base + GEM_NCR, GEM_TX_START);
  }
}

EFI_STATUS
GemReclaim (
  GEM_DMA  *Dma,
  VOID     **Buffer
  )
{
  UINTN   Index   = Dma->TxTail;
  UINT32  Control = Dma->Tx[Index].Control;

  *Buffer = NULL;
  if (!Dma->TxPending || !(Control & GEM_TX_USED)) {
    return EFI_SUCCESS;
  }

  MemoryFence ();
  *Buffer              = Dma->Original[Index];
  Dma->Original[Index] = NULL;
  Dma->TxTail          = (Index + 1) % GEM_TX_COUNT;
  Dma->TxPending--;
  if (Control & (BIT27 | BIT28 | BIT29)) {
    Dma->TxError++;
    return EFI_DEVICE_ERROR;
  }

  Dma->TxGood++;
  return EFI_SUCCESS;
}

BOOLEAN
GemPacketReady (
  GEM_DMA  *Dma
  )
{
  return Dma->Running && !Dma->Faulted && (Dma->Rx[Dma->RxHead].Address & GEM_RX_USED);
}

EFI_STATUS
GemReceive (
  GEM_DMA  *Dma,
  VOID     *Buffer,
  UINTN    *Bytes
  )
{
  UINTN       Index = Dma->RxHead;
  UINT32      Control;
  UINTN       Length;
  EFI_STATUS  Status = EFI_SUCCESS;

  if (!GemPacketReady (Dma)) {
    return EFI_NOT_READY;
  }

  MemoryFence ();
  Control = Dma->Rx[Index].Control;
  Length  = Control & 0x1fff;
  if (((Control & (GEM_RX_SOF | GEM_RX_EOF)) != (GEM_RX_SOF | GEM_RX_EOF)) ||
      (Length < 14) || (Length > 1514))
  {
    Dma->RxError++;
    Status = EFI_DEVICE_ERROR;
  } else {
    UINTN  Capacity = *Bytes;
    *Bytes = Length;
    if (Capacity < Length) {
      return EFI_BUFFER_TOO_SMALL;
    }

    CopyMem (Buffer, Dma->RxBytes + Index * GEM_FRAME_BYTES, Length);
    Dma->RxGood++;
  }

  Dma->Rx[Index].Control = 0;
  MemoryFence ();
  Dma->Rx[Index].Address &= ~GEM_RX_USED;
  MemoryFence ();
  Dma->RxHead = (Index + 1) % GEM_RX_COUNT;
  return Status;
}
