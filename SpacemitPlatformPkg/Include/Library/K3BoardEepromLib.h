/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_BOARD_EEPROM_LIB_H
#define K3_BOARD_EEPROM_LIB_H
#include <Uefi.h>
/* Complete 24C02 access; write the four commit bytes' pages last. */
EFI_STATUS
K3BoardEepromAccess (
  UINT8    *Data,
  BOOLEAN  Write,
  UINTN    CommitOffset
  );

#endif
