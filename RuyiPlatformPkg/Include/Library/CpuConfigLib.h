/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>

typedef struct {
  UINT8 Value;
  CONST CHAR16 *Name;
} CPU_CONFIG_MODE;

typedef struct {
  UINT8 Default;
  CONST CPU_CONFIG_MODE *Modes;
  UINTN Count;
  CONST CHAR16 *Help;
} CPU_CONFIG;

// EFI_UNSUPPORTED means that the board has no selectable CPU mode.
// Modes and Help refer to static platform data and must not be freed.
EFI_STATUS CpuConfigGet (CPU_CONFIG *Config);
EFI_STATUS CpuConfigSet (UINT8 Mode);
