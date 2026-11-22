/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>

/**
  Read registers over an idle DesignWare I2C controller in standard mode.
  Base is the CPU-accessible MMIO address. The caller owns clock, pinmux,
  timing and serialization for the transfer.
**/
EFI_STATUS
EFIAPI
DwI2cReadRegisters (
  IN  UINTN  Base,
  IN  UINT8  Address,
  IN  UINT8  Register,
  OUT UINT8  *Data,
  IN  UINTN  Count
  );
