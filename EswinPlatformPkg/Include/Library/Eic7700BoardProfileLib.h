// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Library/CorebootImageLib.h>

typedef struct {
  COREBOOT_IMAGE_LAYOUT Image;
  CONST CHAR8 *Manufacturer;
  UINT32 FactoryOffset;
  UINT32 FactoryBytes;
  UINTN  EthernetPorts;
  BOOLEAN FastbootCanSwitchHostPort;
  BOOLEAN AllowCpuOppOverride;
  BOOLEAN DualDie;
} EIC7700_BOARD_PROFILE;

CONST EIC7700_BOARD_PROFILE *Eic7700GetBoardProfile (VOID);
