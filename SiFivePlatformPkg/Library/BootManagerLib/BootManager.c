/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BootProgressLib.h>
#include <Guid/EventGroup.h>
#include <Guid/TtyTerm.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SerialIo.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Library/PciGopLoaderLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC CONST struct {
  USB_CLASS_DEVICE_PATH       Keyboard;
  EFI_DEVICE_PATH_PROTOCOL    End;
} mKeyboard = {
  {
    { MESSAGING_DEVICE_PATH, MSG_USB_CLASS_DP,               { sizeof (USB_CLASS_DEVICE_PATH),    0 }
    }, 0xffff, 0xffff, 3, 1, 1
  },
  { END_DEVICE_PATH_TYPE,  END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC VOID  *mGraphicsRegistration;

STATIC VOID EFIAPI
GraphicsAvailable (
  EFI_EVENT  Event,
  VOID       *Context
  )
{
  EFI_HANDLE                Handle;
  EFI_DEVICE_PATH_PROTOCOL  *Path;
  UINTN                     Size;

  for ( ; ;) {
    Size = sizeof (Handle);
    if (EFI_ERROR (
          gBS->LocateHandle (
                 ByRegisterNotify,
                 NULL,
                 mGraphicsRegistration,
                 &Size,
                 &Handle
                 )
          ))
    {
      break;
    }

    Path = DevicePathFromHandle (Handle);
    if ((Path == NULL) || (Handle == gST->ConsoleOutHandle)) {
      continue;
    }

    EfiBootManagerUpdateConsoleVariable (ConOut, Path, NULL);
    EfiBootManagerUpdateConsoleVariable (ErrOut, Path, NULL);
    // A GOP Start() may still be running. Connect console drivers only
    // after ConnectAll() returns, outside the protocol notification.
  }
}

STATIC EFI_STATUS
Application (
  EFI_GUID       *Guid,
  CHAR16         *Name,
  UINT32         Attributes,
  EFI_INPUT_KEY  *Key
  )
{
  EFI_LOADED_IMAGE_PROTOCOL          *Image;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  File;
  EFI_DEVICE_PATH_PROTOCOL           *Path;
  EFI_BOOT_MANAGER_LOAD_OPTION       Option, *Options;
  UINTN                              Count;
  INTN                               Index;
  EFI_STATUS                         Status;

  Status = gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Image);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  EfiInitializeFwVolDevicepathNode (&File, Guid);
  Path = AppendDevicePathNode (DevicePathFromHandle (Image->DeviceHandle), &File.Header);
  if (!Path) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = EfiBootManagerInitializeLoadOption (
             &Option,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             Attributes,
             Name,
             Path,
             NULL,
             0
             );
  FreePool (Path);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Options = EfiBootManagerGetLoadOptions (&Count, LoadOptionTypeBoot);
  Index   = EfiBootManagerFindLoadOption (&Option, Options, Count);
  if (Index < 0) {
    Status = EfiBootManagerAddLoadOptionVariable (&Option, (UINTN)-1);
  } else {
    Option.OptionNumber = Options[Index].OptionNumber;
  }

  if (!EFI_ERROR (Status) && Key) {
    Status = EfiBootManagerAddKeyOptionVariable (NULL, (UINT16)Option.OptionNumber, 0, Key, NULL);
  }

  EfiBootManagerFreeLoadOptions (Options, Count);
  EfiBootManagerFreeLoadOption (&Option);
  return Status;
}

VOID EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  EFI_HANDLE                    *Handles;
  UINTN                         Count;
  EFI_BOOT_MANAGER_LOAD_OPTION  Setup;
  EFI_INPUT_KEY                 F2 = { SCAN_F2, 0 }, Enter = { SCAN_NULL, CHAR_CARRIAGE_RETURN };
  VENDOR_DEVICE_PATH            Terminal = {
    { MESSAGING_DEVICE_PATH, MSG_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 }
    }, EDKII_XTERM_R6_GUID
  };

  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);
  EfiBootManagerDispatchDeferredImages ();
  // Install binding policy before enabling foreign image execution.
  if (!EFI_ERROR (PciGopInitialize ())) {
    PciGopConnectEmulator ();
  }

  EfiCreateProtocolNotifyEvent (
    &gEfiGraphicsOutputProtocolGuid,
    TPL_CALLBACK,
    GraphicsAvailable,
    NULL,
    &mGraphicsRegistration
    );
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSerialIoProtocolGuid, NULL, &Count, &Handles))) {
    for (UINTN Index = 0; Index < Count; Index++) {
      EFI_DEVICE_PATH_PROTOCOL  *Base = DevicePathFromHandle (Handles[Index]);
      EFI_DEVICE_PATH_PROTOCOL  *Path = Base ? AppendDevicePathNode (Base, &Terminal.Header) : NULL;
      if (!Path) {
        continue;
      }

      EfiBootManagerUpdateConsoleVariable (ConIn, Path, NULL);
      EfiBootManagerUpdateConsoleVariable (ConOut, Path, NULL);
      EfiBootManagerUpdateConsoleVariable (ErrOut, Path, NULL);
      FreePool (Path);
    }

    FreePool (Handles);
  }

  EfiBootManagerUpdateConsoleVariable (ConIn, (VOID *)&mKeyboard, NULL);
  EfiBootManagerRegisterContinueKeyOption (0, &Enter, NULL);
  if (!EFI_ERROR (EfiBootManagerGetBootManagerMenu (&Setup))) {
    EfiBootManagerAddKeyOptionVariable (NULL, (UINT16)Setup.OptionNumber, 0, &F2, NULL);
    EfiBootManagerFreeLoadOption (&Setup);
  }
}

VOID EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  EFI_GUID       Shell = {
    0x7c04a583, 0x9e3e, 0x4f1c, { 0xad, 0x65, 0xe0, 0x52, 0x68, 0xd0, 0xb4, 0xd1 }
  };
  EFI_GUID       Menu = {
    0xeec25bdc, 0x67f2, 0x4d95, { 0xb1, 0xd5, 0xf8, 0x1b, 0x20, 0x39, 0xd1, 0x1d }
  };
  EFI_INPUT_KEY  F12 = { SCAN_F12, 0 };

  EfiBootManagerConnectAll ();
  PciGopConnectSelectedDrivers ();
  EfiBootManagerConnectAllDefaultConsoles ();
  EfiBootManagerRefreshAllBootOption ();
  Application (&Shell, L"UEFI Shell", LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP, NULL);
  Application (&Menu, L"Boot Menu", LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP | LOAD_OPTION_HIDDEN, &F12);
  BootProgressStart (FALSE);
}

VOID EFIAPI
PlatformBootManagerWaitCallback (
  UINT16  TimeoutRemain
  )
{
  BootProgressUpdate (TimeoutRemain);
}

VOID EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  Setup;

  BootProgressStop ();

  if (EFI_ERROR (EfiBootManagerGetBootManagerMenu (&Setup))) {
    return;
  }

  for ( ; ;) {
    EfiBootManagerBoot (&Setup);
  }
}
