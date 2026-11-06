/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BootProgressLib.h>
#include <Guid/EventGroup.h>
#include <Protocol/GraphicsOutput.h>
#include <Library/DevicePathLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Library/BootManagerLib.h>
#include <Library/PciGopLoaderLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

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

VOID EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  Setup;
  EFI_INPUT_KEY                 SetupKey    = { SCAN_F2, 0 };
  EFI_INPUT_KEY                 ContinueKey = { SCAN_NULL, CHAR_CARRIAGE_RETURN };

  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);
  EfiBootManagerDispatchDeferredImages ();
  BootManagerConfigureNetworkBoot ();
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
  BootManagerRegisterSerialConsoles ();
  BootManagerRegisterUsbKeyboard ();
  EfiBootManagerRegisterContinueKeyOption (0, &ContinueKey, NULL);
  if (!EFI_ERROR (EfiBootManagerGetBootManagerMenu (&Setup))) {
    EfiBootManagerAddKeyOptionVariable (NULL, (UINT16)Setup.OptionNumber, 0, &SetupKey, NULL);
    EfiBootManagerFreeLoadOption (&Setup);
  }
}

VOID EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  EFI_INPUT_KEY  BootMenuKey = { SCAN_F12, 0 };

  EfiBootManagerConnectAll ();
  PciGopConnectSelectedDrivers ();
  EfiBootManagerConnectAllDefaultConsoles ();
  EfiBootManagerRefreshAllBootOption ();
  BootManagerRegisterFvApplication (
    &gUefiShellFileGuid,
    L"UEFI Shell",
    LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP,
    NULL,
    FALSE
    );
  BootManagerRegisterFvApplication (
    &gRuyiBootMenuFileGuid,
    L"Boot Menu",
    LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP | LOAD_OPTION_HIDDEN,
    &BootMenuKey,
    FALSE
    );
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
