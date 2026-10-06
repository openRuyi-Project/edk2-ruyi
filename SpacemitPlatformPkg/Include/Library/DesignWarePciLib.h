/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef SPACEMIT_DESIGNWARE_PCI_LIB_H
#define SPACEMIT_DESIGNWARE_PCI_LIB_H
#include <Uefi.h>

#define SPACEMIT_PCIE_MAX_PORTS  5

typedef struct {
  UINT64    Cpu;
  UINT64    Pci;
  UINT64    Size;
  UINT32    Flags;
} SPACEMIT_PCIE_RANGE;

typedef struct {
  INT32                  Node;
  UINT16                 Segment;
  UINT8                  BusMin;
  UINT8                  BusMax;
  UINTN                  Dbi;
  UINTN                  Dbi2;
  UINTN                  Atu;
  UINTN                  Config;
  UINTN                  ConfigSize;
  UINTN                  Link;
  UINTN                  Control;
  UINT32                 Lanes;
  UINT32                 MaxSpeed;
  SPACEMIT_PCIE_RANGE    Ranges[3];
  UINT32                 RangeCount;
  BOOLEAN                Ready;
} SPACEMIT_PCIE_PORT;

// Hardware operations use 32-bit controller registers and full-width addresses.
EFI_STATUS
SpacemitPcieProgramWindow (
  SPACEMIT_PCIE_PORT  *Port,
  UINT32              Index,
  UINT32              Type,
  UINT64              Cpu,
  UINT64              Pci,
  UINT64              Size
  );

EFI_STATUS
SpacemitPcieConfigAccess (
  SPACEMIT_PCIE_PORT  *Port,
  UINT8               Bus,
  UINT8               Device,
  UINT8               Function,
  UINT16              Register,
  UINTN               Width,
  BOOLEAN             Write,
  UINT32              *Value
  );

// Parse the standard K3 bindings without performing any MMIO operation.
EFI_STATUS
SpacemitPcieParse (
  CONST VOID          *Fdt,
  SPACEMIT_PCIE_PORT  *Ports,
  UINTN               Capacity,
  UINTN               *Count
  );

#endif
