// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

// Add a class path so USB keyboards can join the input console on discovery.
VOID EFIAPI
BootManagerRegisterUsbKeyboard (
  VOID
  );

// Replace saved terminal types on each discovered UART with TTYTERM.
VOID EFIAPI
BootManagerRegisterSerialConsoles (
  VOID
  );

// Register an installed FV application and reuse its existing boot option.
EFI_STATUS EFIAPI
BootManagerRegisterFvApplication (
  CONST EFI_GUID  *Guid,
  CHAR16          *Title,
  UINT32          Attributes,
  EFI_INPUT_KEY   *Key OPTIONAL,
  BOOLEAN         Launch
  );
