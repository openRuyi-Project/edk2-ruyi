/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>
#define RUYI_SETTING_EMULATION BIT0
#define RUYI_SETTING_PXE       BIT1
#define RUYI_SETTING_HTTP      BIT2
#define RUYI_SETTINGS_MASK     (BIT0 | BIT1 | BIT2)
#define RUYI_SETTINGS_OVERRIDE_NAME L"PlatformSettingsOverride"

typedef struct {
  UINT32 Mask;
  UINT32 Values;
} RUYI_SETTINGS_OVERRIDE;

// Boot-services protocol. Defaults use the platform's persistent storage.
// GetEffective reads the optional UEFI override on every call. Present reports
// a variable's existence even when its contents fail validation.
typedef struct {
  EFI_STATUS (EFIAPI *GetDefaults)(UINT32 *Values);
  EFI_STATUS (EFIAPI *SetDefaults)(UINT32 Values);
  EFI_STATUS (EFIAPI *GetEffective)(UINT32 *Values, BOOLEAN *Present OPTIONAL);
  EFI_STATUS (EFIAPI *ClearOverrides)(VOID);
} RUYI_PLATFORM_SETTINGS_PROTOCOL;
extern EFI_GUID gRuyiPlatformSettingsProtocolGuid;
