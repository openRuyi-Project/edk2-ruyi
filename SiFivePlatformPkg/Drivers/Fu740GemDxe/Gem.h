/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef FU740_GEM_H
#define FU740_GEM_H

#include <Uefi.h>

#define GEM_NCR          0x000
#define GEM_NCFGR        0x004
#define GEM_NSR          0x008
#define GEM_USRIO        0x00c
#define GEM_DMA_CONFIG   0x010
#define GEM_TSR          0x014
#define GEM_RX_QUEUE     0x018
#define GEM_TX_QUEUE     0x01c
#define GEM_RSR          0x020
#define GEM_ISR          0x024
#define GEM_IDR          0x02c
#define GEM_MAN          0x034
#define GEM_HASH_LOW     0x080
#define GEM_HASH_HIGH    0x084
#define GEM_MAC_LOW      0x088
#define GEM_MAC_HIGH     0x08c
#define GEM_DESIGN1      0x280
#define GEM_DESIGN6      0x294
#define GEM_RX_ENABLE    BIT2
#define GEM_TX_ENABLE    BIT3
#define GEM_MDIO_ENABLE  BIT4
#define GEM_CLEAR_STATS  BIT5
#define GEM_TX_START     BIT9
#define GEM_TX_HALT      BIT10
#define GEM_RX_USED      BIT0
#define GEM_RX_WRAP      BIT1
#define GEM_RX_SOF       BIT14
#define GEM_RX_EOF       BIT15
#define GEM_TX_LAST      BIT15
#define GEM_TX_WRAP      BIT30
#define GEM_TX_USED      BIT31
#define GEM_RX_COUNT     2048
#define GEM_TX_COUNT     32
#define GEM_FRAME_BYTES  1536

typedef struct {
  volatile UINT32    Address;
  volatile UINT32    Control;
} GEM_DESCRIPTOR;

typedef struct {
  UINTN      Base, Management;
  UINT32     Phy, PhyId, MdcDivider, BusWidth;
  UINT32     Speed, AppliedSpeed;
  BOOLEAN    Full, Up, AppliedFull;
  UINT64     LastLinkCheck;
} GEM_PORT;

typedef struct {
  GEM_PORT          *Port;
  VOID              *Allocation;
  UINTN             Pages;
  GEM_DESCRIPTOR    *Rx, *Tx, *Dummy;
  UINT8             *RxBytes, *TxBytes;
  VOID              *Original[GEM_TX_COUNT];
  UINTN             RxHead, TxHead, TxTail, TxPending;
  UINT64            RxGood, RxError, TxGood, TxError, TxRestarts;
  BOOLEAN           Running, Faulted;
} GEM_DMA;

BOOLEAN
GemFdtEnabled (
  CONST VOID  *Fdt,
  INT32       Node
  );

EFI_STATUS
GemParse (
  CONST VOID  *Fdt,
  INT32       Node,
  GEM_PORT    *Port
  );

EFI_STATUS
GemPrepare (
  GEM_PORT  *Port
  );

EFI_STATUS
GemPhyConfigure (
  GEM_PORT  *Port
  );

EFI_STATUS
GemMdio (
  GEM_PORT  *Port,
  UINT32    Register,
  BOOLEAN   Read,
  UINT16    *Value
  );

EFI_STATUS
GemLink (
  GEM_PORT  *Port,
  BOOLEAN   *Up,
  BOOLEAN   *Full,
  UINT32    *Speed
  );

VOID
GemSpeed (
  GEM_PORT  *Port,
  UINT32    Speed,
  BOOLEAN   Full
  );

VOID
GemMacAddress (
  GEM_PORT     *Port,
  CONST UINT8  *Mac
  );

EFI_STATUS
GemDmaStart (
  GEM_DMA      *Dma,
  GEM_PORT     *Port,
  CONST UINT8  *Mac
  );

EFI_STATUS
GemDmaStop (
  GEM_DMA  *Dma
  );

EFI_STATUS
GemDmaFree (
  GEM_DMA  *Dma
  );

EFI_STATUS
GemTransmit (
  GEM_DMA  *Dma,
  VOID     *Buffer,
  UINTN    Bytes
  );

EFI_STATUS
GemReclaim (
  GEM_DMA  *Dma,
  VOID     **Buffer
  );

BOOLEAN
GemPacketReady (
  GEM_DMA  *Dma
  );

EFI_STATUS
GemReceive (
  GEM_DMA  *Dma,
  VOID     *Buffer,
  UINTN    *Bytes
  );

VOID
GemRestartTx (
  GEM_DMA  *Dma,
  UINT32   Status
  );

#endif
