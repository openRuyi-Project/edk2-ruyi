/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef EQOS_LIB_H
#define EQOS_LIB_H
#include <Uefi.h>

typedef struct {
  CONST VOID *Fdt;
  INT32 Node, PhyNode;
  UINTN Base;
  UINT32 PhyAddress, AssertUs, DeassertUs, PhyId, CsrRate;
  UINT32 DmaBusMode;
  UINT8 MdioRange, DelayFlags, AddressBits;
  BOOLEAN Aldps, DisableClkout, Owned, Ready, PhyConfigured, Faulted;
  EFI_STATUS Error, RestoreError;
} EQOS_PORT;

// EqosPlatformLib owns clocks, resets, pins and link-speed changes.
INT32 EqosNextNode (CONST VOID *Fdt, INT32 Previous);
// Returns a zero-initialized port allocation that the caller frees with FreePool.
// On failure *Port is NULL and no hardware ownership is acquired.
EFI_STATUS EqosParse (CONST VOID *Fdt, INT32 Node, EQOS_PORT **Port);
EFI_STATUS EqosPrepare (EQOS_PORT *Port);
EFI_STATUS EqosSetLink (EQOS_PORT *Port, UINT32 Speed, BOOLEAN FullDuplex);
// Park only after DMA has stopped. Failed parking retains the allocation.
EFI_STATUS EqosPark (EQOS_PORT *Port);

// EqosLib provides the common Clause22 and RTL8211F operations.
EFI_STATUS EqosMdioRead (EQOS_PORT *Port, UINT32 Register, UINT16 *Value);
EFI_STATUS EqosMdioWrite (EQOS_PORT *Port, UINT32 Register, UINT16 Value);
EFI_STATUS EqosPhyConfigure (EQOS_PORT *Port);
EFI_STATUS EqosLink (EQOS_PORT *Port, BOOLEAN *Up, BOOLEAN *FullDuplex, UINT32 *Speed);
#endif
