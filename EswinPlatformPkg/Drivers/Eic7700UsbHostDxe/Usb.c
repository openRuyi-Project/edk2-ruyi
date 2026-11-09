/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/Eic7700UsbLib.h>
#include <Library/FdtBoardInfoLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>

STATIC BOOLEAN
String (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name, CONST CHAR8 *Value)
{
  CONST CHAR8  *Text;
  INT32        Length;

  Text = FdtGetProp (Fdt, Node, Name, &Length);
  return Text && (Length == AsciiStrSize (Value)) && !AsciiStrCmp (Text, Value);
}


EFI_STATUS EFIAPI
Eic7700UsbHostEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  RUYI_BOARD_INFO  Info;
  CONST VOID      *Fdt;
  INT32           Node = -1;
  EFI_STATUS      Status, Result = EFI_NOT_FOUND;

  Status = FdtBoardInfoGet (&Info, &Fdt);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  while ((Node = FdtNodeOffsetByCompatible (Fdt, Node, "eswin,eic7700-dwc3")) >= 0) {
    if (!FdtNodeEnabled (Fdt, Node) || !String (Fdt, Node, "dr_mode", "host")) {
      continue;
    }
    UINTN Base;
    EFI_HANDLE Handle = NULL;

    Status = Eic7700UsbInitialize (Fdt, Node, FALSE, &Base);
    if (!EFI_ERROR (Status)) {
      Status = RegisterNonDiscoverableMmioDevice (
                 NonDiscoverableDeviceTypeXhci, NonDiscoverableDeviceDmaTypeNonCoherent,
                 NULL, &Handle, 1, Base, (UINTN)EIC7700_XHCI_SIZE);
    }
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "EIC7700 USB host node %d: %r\n", Node, Status));
      if (Result != EFI_SUCCESS) {
        Result = Status;
      }
    } else {
      Result = EFI_SUCCESS;
    }
  }
  return Result;
}
