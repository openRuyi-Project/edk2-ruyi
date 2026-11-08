// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Guid/TtyTerm.h>
#include <Protocol/SerialIo.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BootManagerLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC CONST struct {
  USB_CLASS_DEVICE_PATH       Keyboard;
  EFI_DEVICE_PATH_PROTOCOL    End;
} mUsbKeyboard = {
  {
    { MESSAGING_DEVICE_PATH, MSG_USB_CLASS_DP, { sizeof (USB_CLASS_DEVICE_PATH), 0 } },
    0xffff, 0xffff, 3, 1, 1
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

VOID EFIAPI
BootManagerRegisterUsbKeyboard (
  VOID
  )
{
  EfiBootManagerUpdateConsoleVariable (ConIn, (EFI_DEVICE_PATH_PROTOCOL *)&mUsbKeyboard, NULL);
}

VOID EFIAPI
BootManagerRegisterSerialConsoles (
  VOID
  )
{
  EFI_HANDLE                *Handles;
  UINTN                     Count, Index;
  EFI_STATUS                Status;
  VENDOR_DEVICE_PATH        Terminal;
  EFI_DEVICE_PATH_PROTOCOL  *Path;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSerialIoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  ZeroMem (&Terminal, sizeof (Terminal));
  Terminal.Header.Type    = MESSAGING_DEVICE_PATH;
  Terminal.Header.SubType = MSG_VENDOR_DP;
  SetDevicePathNodeLength (&Terminal.Header, sizeof (Terminal));
  CopyGuid (&Terminal.Guid, &gEfiTtyTermGuid);
  for (Index = 0; Index < Count; Index++) {
    EFI_DEVICE_PATH_PROTOCOL  *Parent = DevicePathFromHandle (Handles[Index]);
    if (Parent == NULL) {
      continue;
    }

    Path = AppendDevicePathNode (Parent, &Terminal.Header);
    if (Path == NULL) {
      continue;
    }

    // Replace any previous terminal type on this UART.
    EfiBootManagerUpdateConsoleVariable (ConIn, Path, Parent);
    EfiBootManagerUpdateConsoleVariable (ConOut, Path, Parent);
    EfiBootManagerUpdateConsoleVariable (ErrOut, Path, Parent);
    FreePool (Path);
  }

  FreePool (Handles);
}
