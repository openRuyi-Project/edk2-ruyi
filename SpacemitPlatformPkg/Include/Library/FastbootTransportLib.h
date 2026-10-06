// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

EFI_STATUS
FastbootUsbStart (
  VOID
  );

EFI_STATUS
FastbootUsbStop (
  VOID
  );

EFI_STATUS
FastbootUsbPoll (
  VOID
  );

EFI_STATUS
FastbootUsbReceive (
  VOID   *Buffer,
  UINTN  *Length
  );

EFI_STATUS
FastbootUsbSend (
  CONST VOID  *Buffer,
  UINTN       Length
  );
