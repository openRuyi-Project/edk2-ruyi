/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef SPACEMIT_PCI_CONTROLLER_PROTOCOL_H
#define SPACEMIT_PCI_CONTROLLER_PROTOCOL_H
#include <Library/DesignWarePciLib.h>

typedef struct {
  UINT32                Revision;
  UINTN                 Count;
  SPACEMIT_PCIE_PORT    *Ports;
  EFI_STATUS (EFIAPI *Config)(UINT16 Segment, UINT8 Bus, UINT8 Device, UINT8 Function,
                              UINT16 Register, UINTN Width, BOOLEAN Write, UINT32 *Value);
} SPACEMIT_PCI_CONTROLLER_PROTOCOL;
extern EFI_GUID  gSpacemitPciControllerProtocolGuid;
#endif
