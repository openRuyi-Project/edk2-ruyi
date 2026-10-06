/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef SPACEMIT_TLV_LIB_H
#define SPACEMIT_TLV_LIB_H
#include <Uefi.h>
#define SPACEMIT_TLV_HEADER_SIZE  11
#define SPACEMIT_TLV_MAC_BASE     0x24
#define SPACEMIT_TLV_MAC_COUNT    0x2a
#define SPACEMIT_TLV_DDR_PART     0x45
#define SPACEMIT_TLV_CRC          0xfe
EFI_STATUS
SpacemitTlvValidate (
  CONST UINT8  *Data,
  UINTN        Capacity,
  UINTN        *Used
  );

EFI_STATUS
SpacemitTlvFind (
  CONST UINT8  *Data,
  UINTN        Capacity,
  UINT8        Code,
  UINTN        *Offset
  );

EFI_STATUS
SpacemitTlvSet (
  UINT8        *Data,
  UINTN        Capacity,
  UINT8        Code,
  CONST UINT8  *Value,
  UINTN        Length
  );

#endif
