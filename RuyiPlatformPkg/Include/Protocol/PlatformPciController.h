/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef RUYI_PCI_CONTROLLER_H
#define RUYI_PCI_CONTROLLER_H
#include <Uefi.h>
#include <Library/PciHostBridgeLib.h>

typedef struct {
  UINT32             Revision;
  PCI_ROOT_BRIDGE    RootBridge;
  EFI_STATUS (EFIAPI *Config)(UINT16 Segment, UINT8 Bus, UINT8 Device,
                              UINT8 Function, UINT16 Register, UINTN Width,
                              BOOLEAN Write, UINT32 *Value);
} RUYI_PCI_CONTROLLER_PROTOCOL;

// Revision 2 publishes all root bridges together before PCI enumeration.
// Revision 1 exposes one embedded RootBridge.
typedef struct {
  RUYI_PCI_CONTROLLER_PROTOCOL    Controller;
  UINTN                           Count;
  CONST PCI_ROOT_BRIDGE           *Bridges;
  // Physical MMIO bases corresponding to the GCD I/O port apertures.
  CONST UINT64                    *IoMmioBases;
} RUYI_MULTI_ROOT_PCI_CONTROLLER;

STATIC inline CONST PCI_ROOT_BRIDGE *
PlatformPciRoots (CONST RUYI_PCI_CONTROLLER_PROTOCOL *Controller, UINTN *Count)
{
  CONST RUYI_MULTI_ROOT_PCI_CONTROLLER *Multi = (CONST VOID *)Controller;
  if (Controller->Revision == 2) {
    *Count = Multi->Count;
    return Multi->Bridges;
  }
  *Count = (Controller->Revision == 1) ? 1 : 0;
  return &Controller->RootBridge;
}

extern EFI_GUID  gRuyiPciControllerProtocolGuid;
#endif
