/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef RUYI_EMULATION_CONFIG_H_
#define RUYI_EMULATION_CONFIG_H_
#include <Uefi.h>

typedef struct {
  EFI_STATUS (EFIAPI *Get)(BOOLEAN *Enabled, BOOLEAN *DefaultEnabled);
  EFI_STATUS (EFIAPI *Set)(BOOLEAN Enabled);
} RUYI_EMULATION_CONFIG_PROTOCOL;

extern EFI_GUID  gRuyiEmulationConfigProtocolGuid;
#endif
