// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

EFI_STATUS
UsbDevicePlatformInit (
  UINTN  *Base
  );

typedef struct {
  UINT16       VendorId;
  UINT16       ProductId;
  CONST CHAR8  *Manufacturer;
  CONST CHAR8  *Product;
} USB_DEVICE_INFO;

CONST USB_DEVICE_INFO *
UsbDevicePlatformGetInfo (
  VOID
  );

// Called only after the device controller and its DMA have stopped.
EFI_STATUS
UsbDevicePlatformStop (
  VOID
  );
