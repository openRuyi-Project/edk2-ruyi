/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/DxeServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/PciIo.h>
#include <Protocol/PciRootBridgeIo.h>
#include <IndustryStandard/Pci.h>
#include <Uefi/UefiInternalFormRepresentation.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BootProgressLib.h>
#include <Library/PlatformBootPolicyLib.h>
#include <Library/PciGopLoaderLib.h>
#include <Library/PcdLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Library/BootManagerLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC VOID     *mGraphicsRegistration;
STATIC BOOLEAN  mGraphicsConfigured;

STATIC VOID
UpdateConsoleGeometry (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  UINTN                         Columns, Rows;

  if (EFI_ERROR (
        gBS->HandleProtocol (
               gST->ConsoleOutHandle,
               &gEfiGraphicsOutputProtocolGuid,
               (VOID **)&Gop
               )
        ) ||
      (Gop->Mode == NULL) || (Gop->Mode->Info == NULL) ||
      EFI_ERROR (
        gST->ConOut->QueryMode (
                       gST->ConOut,
                       gST->ConOut->Mode->Mode,
                       &Columns,
                       &Rows
                       )
        ))
  {
    return;
  }

  // ConsoleSplitter owns the common graphics and text modes for all outputs.
  PcdSet32S (PcdVideoHorizontalResolution, Gop->Mode->Info->HorizontalResolution);
  PcdSet32S (PcdVideoVerticalResolution, Gop->Mode->Info->VerticalResolution);
  PcdSet32S (PcdSetupVideoHorizontalResolution, Gop->Mode->Info->HorizontalResolution);
  PcdSet32S (PcdSetupVideoVerticalResolution, Gop->Mode->Info->VerticalResolution);
  PcdSet32S (PcdConOutColumn, (UINT32)Columns);
  PcdSet32S (PcdConOutRow, (UINT32)Rows);
  PcdSet32S (PcdSetupConOutColumn, (UINT32)Columns);
  PcdSet32S (PcdSetupConOutRow, (UINT32)Rows);
}

STATIC VOID EFIAPI
GraphicsAvailable (
  EFI_EVENT  Event,
  VOID       *Context
  )
{
  EFI_HANDLE                    Handle;
  EFI_DEVICE_PATH_PROTOCOL      *Path;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  UINTN                         Size;
  UINT32                        Width, Height, Scale;

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

    if (!mGraphicsConfigured && !EFI_ERROR (
                                   gBS->HandleProtocol (
                                          Handle,
                                          &gEfiGraphicsOutputProtocolGuid,
                                          (VOID **)&Gop
                                          )
                                   ) &&
        (Gop->Mode != NULL) && (Gop->Mode->Info != NULL))
    {
      Width  = Gop->Mode->Info->HorizontalResolution;
      Height = Gop->Mode->Info->VerticalResolution;
      Scale  = MAX (
                 1,
                 MIN (
                   PcdGet32 (PcdConOutGlyphScale),
                   MIN (Width / (80 * EFI_GLYPH_WIDTH), Height / (25 * EFI_GLYPH_HEIGHT))
                   )
                 );
      // Seed the first console from EDID; later outputs use the common modes.
      PcdSet32S (PcdVideoHorizontalResolution, Width);
      PcdSet32S (PcdVideoVerticalResolution, Height);
      PcdSet32S (PcdSetupVideoHorizontalResolution, Width);
      PcdSet32S (PcdSetupVideoVerticalResolution, Height);
      PcdSet32S (PcdConOutColumn, Width / (EFI_GLYPH_WIDTH * Scale));
      PcdSet32S (PcdConOutRow, Height / (EFI_GLYPH_HEIGHT * Scale));
      PcdSet32S (PcdSetupConOutColumn, Width / (EFI_GLYPH_WIDTH * Scale));
      PcdSet32S (PcdSetupConOutRow, Height / (EFI_GLYPH_HEIGHT * Scale));
      mGraphicsConfigured = TRUE;
    }

    EfiBootManagerUpdateConsoleVariable (ConOut, Path, NULL);
    EfiBootManagerUpdateConsoleVariable (ErrOut, Path, NULL);
    // This notification can interrupt a GOP Start() or mode update, including
    // a later ConnectAll() from Setup. Let the caller finish device connection
    // before any console driver consumes the newly published GOP state.
  }
}

STATIC VOID
ConnectGraphics (
  VOID
  )
{
  EFI_HANDLE           *Handles;
  UINTN                Count;
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  PCI_TYPE00           Pci;

  // Enumerate PCI without starting storage, network or USB drivers.
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiPciRootBridgeIoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (!EFI_ERROR (Status)) {
    for (UINTN Index = 0; Index < Count; Index++) {
      gBS->ConnectController (Handles[Index], NULL, NULL, FALSE);
    }

    FreePool (Handles);
  }

  PciGopConnectSelectedDrivers ();
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiPciIoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  for (UINTN Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiPciIoProtocolGuid, (VOID **)&PciIo);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, 0, sizeof (Pci) / sizeof (UINT32), &Pci);
    if (!EFI_ERROR (Status) && IS_PCI_DISPLAY (&Pci)) {
      gBS->ConnectController (Handles[Index], NULL, NULL, FALSE);
    }
  }

  FreePool (Handles);
  // BDS connects the recorded GOP paths after this initialization returns.
}

VOID EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);
  gDS->Dispatch ();
  EfiBootManagerDispatchDeferredImages ();
  BootManagerConfigureNetworkBoot ();

  // Install binding guards before foreign drivers become executable.
  EFI_STATUS  Status = PciGopInitialize ();
  if (!EFI_ERROR (Status)) {
    PciGopConnectEmulator ();
  } else if (Status != EFI_UNSUPPORTED) {
    DEBUG ((DEBUG_ERROR, "GOP policy initialization failed: %r\n", Status));
  }


  BootManagerRegisterUsbKeyboard ();

  EfiCreateProtocolNotifyEvent (
    &gEfiGraphicsOutputProtocolGuid,
    TPL_CALLBACK,
    GraphicsAvailable,
    NULL,
    &mGraphicsRegistration
    );
  ConnectGraphics ();
  BootManagerRegisterSerialConsoles ();
}

VOID EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  Menu;
  EFI_INPUT_KEY                 SetupKey    = { SCAN_F2, 0 };
  EFI_INPUT_KEY                 BootMenuKey = { SCAN_F12, 0 };
  EFI_INPUT_KEY                 FastbootKey = { SCAN_F9, 0 };
  EFI_INPUT_KEY                 ContinueKey = { SCAN_NULL, CHAR_CARRIAGE_RETURN };
  EFI_STATUS                    Status;
  EFI_STATUS FastbootStatus = BootManagerRegisterFvApplication (
    &gRuyiFastbootFileGuid, L"USB Fastboot", LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP,
    &FastbootKey, FALSE);
  BOOLEAN FastbootAvailable = !EFI_ERROR (FastbootStatus) || FastbootStatus == EFI_ALREADY_STARTED;

  // The early graphics pass has returned and BDS has connected its consoles.
  UpdateConsoleGeometry ();
  if (PlatformBootPolicyRecovery ()) {
    BootProgressStop ();
    gST->ConOut->SetAttribute (gST->ConOut, EFI_WHITE | EFI_BACKGROUND_BLACK);
    gST->ConOut->ClearScreen (gST->ConOut);
    Print (L"RAM Recovery\r\nInitializing devices for USB Fastboot...\r\n");
  } else {
    BootProgressStart (FastbootAvailable);
  }

  EfiBootManagerConnectAll ();
  PciGopConnectSelectedDrivers ();
  EfiBootManagerConnectAllDefaultConsoles ();
  UpdateConsoleGeometry ();
  Status = PlatformBootPolicyInstall ();
  DEBUG ((DEBUG_INFO, "Storage boot policy: %r\n", Status));
  EfiBootManagerRefreshAllBootOption ();
  BootManagerRegisterFvApplication (
    &gUefiShellFileGuid,
    L"UEFI Shell",
    LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP,
    NULL,
    FALSE
    );
  BootManagerRegisterFvApplication (
    &gRuyiFastbootFileGuid,
    L"USB Fastboot",
    LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP,
    &FastbootKey,
    PlatformBootPolicyRecovery ()
    );

  // F12 launches this application; it is not a selectable boot target.
  BootManagerRegisterFvApplication (
    &gRuyiBootMenuFileGuid,
    L"Boot Menu",
    LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP | LOAD_OPTION_HIDDEN,
    &BootMenuKey,
    FALSE
    );
  Status = EfiBootManagerGetBootManagerMenu (&Menu);
  if (!EFI_ERROR (Status)) {
    Status = EfiBootManagerAddKeyOptionVariable (
               NULL,
               (UINT16)Menu.OptionNumber,
               0,
               &SetupKey,
               NULL
               );
    DEBUG ((DEBUG_INFO, "Press F2 for firmware setup (%r)\n", Status));
    EfiBootManagerFreeLoadOption (&Menu);
  }

  EfiBootManagerRegisterContinueKeyOption (0, &ContinueKey, NULL);
  if (PlatformBootPolicyRecovery ()) {
    gST->ConOut->SetAttribute (gST->ConOut, EFI_WHITE | EFI_BACKGROUND_BLACK);
    gST->ConOut->ClearScreen (gST->ConOut);
    BootProgressStart (FastbootAvailable);
  }
}

VOID EFIAPI
PlatformBootManagerWaitCallback (
  IN UINT16  TimeoutRemain
  )
{
  BootProgressUpdate (TimeoutRemain);
}

VOID EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  Menu;
  EFI_STATUS                    Status;

  BootProgressStop ();
  DEBUG ((DEBUG_WARN, "No usable UEFI boot option; opening setup\n"));
  Status = EfiBootManagerGetBootManagerMenu (&Menu);
  if (EFI_ERROR (Status)) {
    return;
  }

  do {
    EfiBootManagerBoot (&Menu);
  } while (!EFI_ERROR (Menu.Status));

  EfiBootManagerFreeLoadOption (&Menu);
}
