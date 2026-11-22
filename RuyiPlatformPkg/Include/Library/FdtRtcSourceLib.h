/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>

/**
  Sample the hardware RTC described by a validated device tree during boot.
  Return EFI_SUCCESS with a valid EFI_TIME, or an error if the source cannot
  supply a time. The caller does not retain Fdt or access the source at runtime.
**/
EFI_STATUS
EFIAPI
FdtRtcReadTime (
  IN  CONST VOID  *Fdt,
  OUT EFI_TIME    *Time
  );
