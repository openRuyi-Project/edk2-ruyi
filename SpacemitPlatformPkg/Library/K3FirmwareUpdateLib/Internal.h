/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_FIRMWARE_UPDATE_INTERNAL_H
#define K3_FIRMWARE_UPDATE_INTERNAL_H

#include <Uefi.h>

#define FMAP_OFFSET  0x1b0000U
#define FMAP_SIZE    (56 + 10 * 42)

BOOLEAN
K3FirmwareMapValid (
  CONST UINT8  *Map
  );

#endif
