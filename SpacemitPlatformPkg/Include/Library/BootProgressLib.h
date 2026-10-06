// SPDX-License-Identifier: BSD-2-Clause-Patent

#ifndef RISCV_BOOT_PROGRESS_LIB_H
#define RISCV_BOOT_PROGRESS_LIB_H

#include <Uefi.h>

// Start the logo and serial countdown after connecting consoles.
VOID EFIAPI
BootProgressStart (
  VOID
  );

// End the startup logo/countdown before launching a firmware application.
VOID EFIAPI
BootProgressStop (
  VOID
  );

// Display the time remaining in the standard BDS hotkey wait.
VOID EFIAPI
BootProgressUpdate (
  UINT16  Remaining
  );

#endif
