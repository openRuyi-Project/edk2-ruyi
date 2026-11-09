/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef EQOS_DMA_LIB_H
#define EQOS_DMA_LIB_H
#include <Library/EqosLib.h>
#define EQOS_TX           8U
#define EQOS_RX           16U
#define EQOS_FRAME_BYTES  1600U
typedef struct {
  volatile UINT32    Word[4];
} EQOS_DESC;
typedef struct {
  EQOS_PORT            *Port;
  VOID                    *Allocation, *Mapping;
  UINTN                   Pages;
  EFI_PHYSICAL_ADDRESS    Bus;
  EQOS_DESC            *Tx, *Rx;
  UINT8                   *TxBytes, *RxBytes;
  UINTN                   TxNext, TxHead, TxCount, RxHead;
  VOID                    *Original[EQOS_TX];
  BOOLEAN                 Running, Faulted;
  UINT64                  TxGood, TxError, RxGood, RxError;
} EQOS_DMA;
EFI_STATUS
EqosDmaStart (
  EQOS_DMA   *Dma,
  EQOS_PORT  *Port,
  CONST UINT8   *Mac
  );

EFI_STATUS
EqosDmaStop (
  EQOS_DMA  *Dma
  );

EFI_STATUS
EqosDmaFree (
  EQOS_DMA  *Dma
  );

EFI_STATUS
EqosTransmit (
  EQOS_DMA  *Dma,
  VOID         *Buffer,
  UINTN        Bytes
  );

EFI_STATUS
EqosReclaim (
  EQOS_DMA  *Dma,
  VOID         **Buffer
  );

EFI_STATUS
EqosReceive (
  EQOS_DMA  *Dma,
  VOID         *Buffer,
  UINTN        *Bytes
  );

BOOLEAN
EqosPacketReady (
  EQOS_DMA  *Dma
  );

VOID
EqosMacAddress (
  EQOS_PORT  *Port,
  CONST UINT8   *Mac
  );

VOID
EqosSpeed (
  EQOS_PORT  *Port,
  UINT32        Speed,
  BOOLEAN       Full
  );

#endif
