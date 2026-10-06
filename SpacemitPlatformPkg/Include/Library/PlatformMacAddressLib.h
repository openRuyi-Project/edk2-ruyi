/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef PLATFORM_MAC_ADDRESS_LIB_H
#define PLATFORM_MAC_ADDRESS_LIB_H
#include <Uefi.h>
/* Board-owned allocation index; no NIC fuse/register identity is read. */
EFI_STATUS
PlatformGetMacAddress (
  UINTN  Index,
  UINT8  Mac[6]
  );

#endif
