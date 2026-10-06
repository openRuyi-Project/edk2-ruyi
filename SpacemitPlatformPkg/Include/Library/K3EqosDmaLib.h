/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_EQOS_DMA_LIB_H
#define K3_EQOS_DMA_LIB_H
#include <Library/K3EqosLib.h>
#define K3_EQOS_TX           8U
#define K3_EQOS_RX           16U
#define K3_EQOS_FRAME_BYTES  1600U
typedef struct {
  volatile UINT32    Word[4];
} K3_EQOS_DESC;
typedef struct {
  K3_EQOS_PORT            *Port;
  VOID                    *Allocation, *Mapping;
  UINTN                   Pages;
  EFI_PHYSICAL_ADDRESS    Bus;
  K3_EQOS_DESC            *Tx, *Rx;
  UINT8                   *TxBytes, *RxBytes;
  UINTN                   TxNext, TxHead, TxCount, RxHead;
  VOID                    *Original[K3_EQOS_TX];
  BOOLEAN                 Running, Faulted;
  UINT64                  TxGood, TxError, RxGood, RxError;
} K3_EQOS_DMA;
EFI_STATUS
K3EqosDmaStart (
  K3_EQOS_DMA   *Dma,
  K3_EQOS_PORT  *Port,
  CONST UINT8   *Mac
  );

EFI_STATUS
K3EqosDmaStop (
  K3_EQOS_DMA  *Dma
  );

EFI_STATUS
K3EqosDmaFree (
  K3_EQOS_DMA  *Dma
  );

EFI_STATUS
K3EqosTransmit (
  K3_EQOS_DMA  *Dma,
  VOID         *Buffer,
  UINTN        Bytes
  );

EFI_STATUS
K3EqosReclaim (
  K3_EQOS_DMA  *Dma,
  VOID         **Buffer
  );

EFI_STATUS
K3EqosReceive (
  K3_EQOS_DMA  *Dma,
  VOID         *Buffer,
  UINTN        *Bytes
  );

BOOLEAN
K3EqosPacketReady (
  K3_EQOS_DMA  *Dma
  );

VOID
K3EqosMacAddress (
  K3_EQOS_PORT  *Port,
  CONST UINT8   *Mac
  );

VOID
K3EqosSpeed (
  K3_EQOS_PORT  *Port,
  UINT32        Speed,
  BOOLEAN       Full
  );

#endif
