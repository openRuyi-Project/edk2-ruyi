/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef K3_I2C_LIB_H
#define K3_I2C_LIB_H

#include <Uefi.h>

// The caller owns the controller and its clocks, pinmux and timing.
// On failure, the caller must abort its transaction before releasing them.
EFI_STATUS
K3I2cWaitIdle (
  UINTN  Base
  );

EFI_STATUS
K3I2cReadRegisters (
  UINTN  Base,
  UINT8  Address,
  UINT8  Register,
  UINT8  *Data,
  UINTN  Count
  );

EFI_STATUS
K3I2cWriteRegisters (
  UINTN        Base,
  UINT8        Address,
  UINT8        Register,
  CONST UINT8  *Data,
  UINTN        Count
  );

#endif
