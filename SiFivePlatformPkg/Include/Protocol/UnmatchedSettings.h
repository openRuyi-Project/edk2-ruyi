/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef UNMATCHED_SETTINGS_H_
#define UNMATCHED_SETTINGS_H_
#include <Uefi.h>

typedef struct {
  EFI_STATUS (EFIAPI *Get)(BOOLEAN *Enabled, BOOLEAN *DefaultEnabled);
  EFI_STATUS (EFIAPI *Set)(BOOLEAN Enabled);
} UNMATCHED_SETTINGS_PROTOCOL;

extern EFI_GUID  gUnmatchedSettingsProtocolGuid;
#endif
