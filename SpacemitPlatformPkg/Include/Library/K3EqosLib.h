/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_EQOS_LIB_H
#define K3_EQOS_LIB_H
#include <Uefi.h>
#include <Library/K3HardwareLib.h>
typedef struct {
  CONST VOID    *Fdt;
  INT32         Node, PhyNode;
  UINTN         Base, Apmu, Control, Dline, Pll, Mpmu;
  K3_GPIO       Reset;
  UINT32        PhyAddress, AssertUs, DeassertUs, PhyId, CsrRate;
  UINT32        TxDelay, RxDelay;
  UINT8         MdioRange, DelayFlags, AddressBits;
  BOOLEAN       Aldps, DisableClkout, Owned, Ready, PhyConfigured, Faulted, ParentAdded;
  EFI_STATUS    Error, RestoreError;
} K3_EQOS_PORT;
EFI_STATUS
K3EqosParse (
  CONST VOID    *Fdt,
  INT32         Node,
  K3_EQOS_PORT  *Port
  );

EFI_STATUS
K3EqosPrepare (
  K3_EQOS_PORT  *Port
  );

EFI_STATUS
K3EqosMdioRead (
  K3_EQOS_PORT  *Port,
  UINT32        Register,
  UINT16        *Value
  );

EFI_STATUS
K3EqosMdioWrite (
  K3_EQOS_PORT  *Port,
  UINT32        Register,
  UINT16        Value
  );

EFI_STATUS
K3EqosPhyConfigure (
  K3_EQOS_PORT  *Port
  );

EFI_STATUS
K3EqosLink (
  K3_EQOS_PORT  *Port,
  BOOLEAN       *Up,
  BOOLEAN       *FullDuplex,
  UINT32        *Speed
  );

EFI_STATUS
K3EqosPark (
  K3_EQOS_PORT  *Port
  );

#endif
