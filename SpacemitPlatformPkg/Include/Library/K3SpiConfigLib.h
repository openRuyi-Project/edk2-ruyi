/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_SPI_CONFIG_LIB_H_
#define K3_SPI_CONFIG_LIB_H_
#include <Uefi.h>

// Separate 4 KiB sector immediately before CPU configuration in EARLY_RESERVED.
#define K3_EMULATION_CONFIG_OFFSET  0x20e000

typedef enum {
  K3SettingCpuMode,
  K3SettingDriverEmulation,
  K3SettingMax
} K3_SPI_SETTING;

EFI_STATUS
K3SpiConfigGet (
  K3_SPI_SETTING  Setting,
  UINT32          *Value
  );

EFI_STATUS
K3SpiConfigSet (
  K3_SPI_SETTING  Setting,
  UINT32          Value
  );

#endif
