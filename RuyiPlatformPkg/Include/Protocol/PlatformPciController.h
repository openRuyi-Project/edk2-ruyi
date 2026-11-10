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

extern EFI_GUID  gRuyiPciControllerProtocolGuid;
#endif
