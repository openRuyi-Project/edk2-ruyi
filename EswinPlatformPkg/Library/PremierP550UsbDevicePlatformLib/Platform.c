/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/NonDiscoverableDevice.h>
#include <Protocol/NonDiscoverableDevice.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/Eic7700UsbLib.h>
#include <Library/FdtBoardInfoLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UsbDevicePlatformLib.h>

#define USB0_HSP_BUS  0x800

STATIC CONST VOID  *mFdt;
STATIC INT32       mNode;
STATIC EFI_HANDLE  mHost;
STATIC BOOLEAN     mRestoreHost;
STATIC BOOLEAN     mActive;

STATIC EFI_STATUS
FindHost (
  UINTN       Base,
  EFI_HANDLE  *Result
  )
{
  EFI_HANDLE               *Handles;
  UINTN                    Count, Index;
  NON_DISCOVERABLE_DEVICE  *Device;
  EFI_STATUS               Status;

  *Result = NULL;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEdkiiNonDiscoverableDeviceProtocolGuid,
                                    NULL, &Count, &Handles);
  if (Status == EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEdkiiNonDiscoverableDeviceProtocolGuid,
                                 (VOID **)&Device);
    if (EFI_ERROR (Status) || !CompareGuid (Device->Type, &gEdkiiNonDiscoverableXhciDeviceGuid) ||
        (Device->Resources == NULL) || (Device->Resources->AddrRangeMin != Base))
    {
      continue;
    }
    *Result = Handles[Index];
    break;
  }
  FreePool (Handles);
  return EFI_SUCCESS;
}

EFI_STATUS
UsbDevicePlatformInit (
  UINTN  *Base
  )
{
  RUYI_BOARD_INFO  Info;
  CONST UINT32    *Hsp;
  CONST CHAR8     *Mode;
  INT32           Node = -1, Length;
  EFI_STATUS      Status, Cleanup;

  if (mActive || (Base == NULL)) {
    return mActive ? EFI_ALREADY_STARTED : EFI_INVALID_PARAMETER;
  }
  Status = FdtBoardInfoGet (&Info, &mFdt);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  mNode = -1;
  while ((Node = FdtNodeOffsetByCompatible (mFdt, Node, "eswin,eic7700-dwc3")) >= 0) {
    if (!FdtNodeEnabled (mFdt, Node)) {
      continue;
    }
    Hsp = FdtGetProp (mFdt, Node, "eswin,hsp-sp-csr", &Length);
    if ((Hsp != NULL) && (Length == 3 * sizeof (UINT32)) &&
        (Fdt32ToCpu (Hsp[1]) == USB0_HSP_BUS))
    {
      mNode = Node;
      break;
    }
  }
  if (mNode < 0) {
    return EFI_NOT_FOUND;
  }
  Mode = FdtGetProp (mFdt, mNode, "dr_mode", &Length);
  if ((Mode == NULL) ||
      !(((Length == sizeof ("host")) && !AsciiStrCmp (Mode, "host")) ||
        ((Length == sizeof ("peripheral")) && !AsciiStrCmp (Mode, "peripheral"))))
  {
    return EFI_UNSUPPORTED;
  }
  mRestoreHost = AsciiStrCmp (Mode, "host") == 0;
  Status = FdtGetMmio (mFdt, mNode, 0, SIZE_64KB, Base, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = FindHost (*Base, &mHost);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (mHost != NULL) {
    // Stop xHCI and all child devices before changing role or resetting PHYs.
    Status = gBS->DisconnectController (mHost, NULL, NULL);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    mRestoreHost = TRUE;
  }
  mActive = TRUE;
  Status = Eic7700UsbInitialize (mFdt, mNode, TRUE, Base);
  if (EFI_ERROR (Status)) {
    Cleanup = UsbDevicePlatformStop ();
    return EFI_ERROR (Cleanup) ? Cleanup : Status;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
UsbDevicePlatformStop (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Base;

  if (!mActive) {
    return EFI_SUCCESS;
  }
  if (!mRestoreHost) {
    mActive = FALSE;
    return EFI_SUCCESS;
  }
  Status = Eic7700UsbInitialize (mFdt, mNode, FALSE, &Base);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  mActive = FALSE;
  if (mHost == NULL) {
    Status = RegisterNonDiscoverableMmioDevice (
               NonDiscoverableDeviceTypeXhci, NonDiscoverableDeviceDmaTypeNonCoherent,
               NULL, &mHost, 1, Base, (UINTN)EIC7700_XHCI_SIZE);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  Status = gBS->ConnectController (mHost, NULL, NULL, TRUE);
  DEBUG ((DEBUG_INFO, "P550 USB0 host reconnect: %r\n", Status));
  return Status;
}

CONST USB_DEVICE_INFO *
UsbDevicePlatformGetInfo (
  VOID
  )
{
  STATIC CONST USB_DEVICE_INFO Info = {
    0x3452, 0x7700, "ESWIN", "Ruyi Firmware EIC7700"
  };
  return &Info;
}
