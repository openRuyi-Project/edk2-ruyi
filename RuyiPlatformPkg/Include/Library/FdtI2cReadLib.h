/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>

/**
  Read Count bytes from an I2C peripheral in one boot-time transaction.
  Controller is its parent node in Fdt. Address is a 7-bit slave address.
  Write the register index, issue a repeated START, and STOP after the last byte.
  The platform instance manages the controller's resources and releases them
  before returning. It must leave a busy controller alone.
**/
EFI_STATUS
EFIAPI
FdtI2cReadRegisters (
  IN  CONST VOID  *Fdt,
  IN  INT32       Controller,
  IN  UINT8       Address,
  IN  UINT8       Register,
  OUT UINT8       *Data,
  IN  UINTN       Count
  );
