/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Protocol/SerialIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Uefi/UefiInternalFormRepresentation.h>
#include <Protocol/FirmwareVolume2.h>
#include <Guid/TtyTerm.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BootProgressLib.h>
#include <Library/HobLib.h>
#include <Pi/PiBootMode.h>
#include <Library/PcdLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

EFI_STATUS
InstallStorageBootPolicy (
  VOID
  );

#ifdef K3_USB_HOST_STACK
STATIC CONST struct {
  USB_CLASS_DEVICE_PATH       Keyboard;
  EFI_DEVICE_PATH_PROTOCOL    End;
} mUsbKeyboard = {
  {
    { MESSAGING_DEVICE_PATH, MSG_USB_CLASS_DP,               { sizeof (USB_CLASS_DEVICE_PATH),    0 }
    },
    0xffff, 0xffff, 3, 1, 1
  },
  { END_DEVICE_PATH_TYPE,  END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};
#endif

STATIC VOID     *mGraphicsRegistration;
STATIC BOOLEAN  mConsoleReady;
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
    if (mConsoleReady) {
      EfiBootManagerConnectAllDefaultConsoles ();
      UpdateConsoleGeometry ();
    }
  }
}

#ifdef K3_X64EMU
STATIC VOID
ConnectX64Emulator (
  VOID
  )
{
  EFI_HANDLE  *Handles;
  UINTN       Count;
  EFI_STATUS  Status;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gMultiArchUefiEmulatorDeviceGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "X64 UEFI emulator unavailable: %r\n", Status));
    return;
  }

  for (UINTN Index = 0; Index < Count; Index++) {
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY  *Info;
    UINTN                                OpenCount;
    BOOLEAN                              Connected = FALSE;

    Status = gBS->OpenProtocolInformation (
                    Handles[Index],
                    &gMultiArchUefiEmulatorDeviceGuid,
                    &Info,
                    &OpenCount
                    );
    if (!EFI_ERROR (Status)) {
      for (UINTN I = 0; I < OpenCount; I++) {
        if (Info[I].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) {
          Connected = TRUE;
        }
      }

      FreePool (Info);
    }

    if (Connected) {
      continue;
    }

    Status = gBS->ConnectController (Handles[Index], NULL, NULL, FALSE);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "X64 UEFI emulator start failed: %r\n", Status));
    }
  }

  FreePool (Handles);
}

#endif

VOID EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  EFI_HANDLE                *Handles;
  UINTN                     Count, Index;
  EFI_STATUS                Status;
  VENDOR_DEVICE_PATH        Terminal;
  EFI_DEVICE_PATH_PROTOCOL  *Path;

  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);
  EfiBootManagerDispatchDeferredImages ();

 #ifdef K3_X64EMU
  // Install foreign-image support before PCI drivers process Option ROMs.
  ConnectX64Emulator ();
 #endif

 #ifdef K3_USB_HOST_STACK
  EfiBootManagerUpdateConsoleVariable (ConIn, (EFI_DEVICE_PATH_PROTOCOL *)&mUsbKeyboard, NULL);
 #endif

  EfiCreateProtocolNotifyEvent (
    &gEfiGraphicsOutputProtocolGuid,
    TPL_CALLBACK,
    GraphicsAvailable,
    NULL,
    &mGraphicsRegistration
    );
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
  CopyGuid (&Terminal.Guid, &gEdkiiXtermR6Guid);
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

STATIC VOID
RegisterFvApplication (
  CONST EFI_GUID  *Guid,
  CHAR16          *Title,
  UINT32          Attributes,
  EFI_INPUT_KEY   *Key OPTIONAL,
  BOOLEAN         Launch
  )
{
  EFI_DEVICE_PATH_PROTOCOL           *Path;
  EFI_BOOT_MANAGER_LOAD_OPTION       Option;
  EFI_BOOT_MANAGER_LOAD_OPTION       *Existing;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  File;
  EFI_STATUS                         Status;
  EFI_HANDLE                         *Handles;
  UINTN                              Count, Index, FileSize;
  UINTN                              ExistingCount;
  EFI_FIRMWARE_VOLUME2_PROTOCOL      *Fv;
  EFI_FV_FILETYPE                    FileType;
  EFI_FV_FILE_ATTRIBUTES             FileAttributes;
  UINT32                             Authentication;

  EfiInitializeFwVolDevicepathNode (&File, Guid);
  Path   = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&Fv
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    FileSize = 0;
    Status   = Fv->ReadFile (
                     Fv,
                     Guid,
                     NULL,
                     &FileSize,
                     &FileType,
                     &FileAttributes,
                     &Authentication
                     );
    if (!EFI_ERROR (Status) && (FileType == EFI_FV_FILETYPE_APPLICATION)) {
      Path = AppendDevicePathNode (DevicePathFromHandle (Handles[Index]), &File.Header);
      break;
    }
  }

  FreePool (Handles);
  if (Path == NULL) {
    return;
  }

  Status = EfiBootManagerInitializeLoadOption (
             &Option,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             Attributes,
             Title,
             Path,
             NULL,
             0
             );
  if (!EFI_ERROR (Status)) {
    Existing = EfiBootManagerGetLoadOptions (&ExistingCount, LoadOptionTypeBoot);
    INTN  Match = EfiBootManagerFindLoadOption (&Option, Existing, ExistingCount);
    if (Match == -1) {
      Status = EfiBootManagerAddLoadOptionVariable (&Option, MAX_UINTN);
    } else {
      Option.OptionNumber = Existing[Match].OptionNumber;
    }

    if (!EFI_ERROR (Status) && (Key != NULL)) {
      Status = EfiBootManagerAddKeyOptionVariable (
                 NULL,
                 (UINT16)Option.OptionNumber,
                 0,
                 Key,
                 NULL
                 );
    }

    if (Existing != NULL) {
      EfiBootManagerFreeLoadOptions (Existing, ExistingCount);
    }

    DEBUG ((DEBUG_INFO, "%s boot option: %r\n", Title, Status));
    if (!EFI_ERROR (Status) && Launch) {
      BootProgressStop ();
      EfiBootManagerBoot (&Option);
    }

    EfiBootManagerFreeLoadOption (&Option);
  }

  FreePool (Path);
}

VOID EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  CONST EFI_GUID                ShellGuid = {
    0x7c04a583, 0x9e3e, 0x4f1c,
    { 0xad,     0x65,   0xe0,  0x52,0x68, 0xd0, 0xb4, 0xd1 }
  };
  EFI_BOOT_MANAGER_LOAD_OPTION  Menu;
  EFI_INPUT_KEY                 SetupKey    = { SCAN_F2, 0 };
  EFI_INPUT_KEY                 BootMenuKey = { SCAN_F12, 0 };
  EFI_INPUT_KEY                 FastbootKey = { SCAN_F9, 0 };
  EFI_INPUT_KEY                 ContinueKey = { SCAN_NULL, CHAR_CARRIAGE_RETURN };
  EFI_STATUS                    Status;

  // GOP notifications can run inside a display driver's Start().
  // Complete device connection before attaching the graphics console.
  mConsoleReady = FALSE;
  UpdateConsoleGeometry ();
  if (GetBootModeHob () == BOOT_IN_RECOVERY_MODE) {
    BootProgressStop ();
    gST->ConOut->SetAttribute (gST->ConOut, EFI_WHITE | EFI_BACKGROUND_BLACK);
    gST->ConOut->ClearScreen (gST->ConOut);
    Print (L"RAM Recovery\r\nInitializing devices for USB Fastboot...\r\n");
  } else {
    BootProgressStart ();
  }

  EfiBootManagerConnectAll ();
  mConsoleReady = TRUE;
  EfiBootManagerConnectAllDefaultConsoles ();
  UpdateConsoleGeometry ();
  Status = InstallStorageBootPolicy ();
  DEBUG ((DEBUG_INFO, "UFS boot policy: %r\n", Status));
  EfiBootManagerRefreshAllBootOption ();
  RegisterFvApplication (
    &ShellGuid,
    L"UEFI Shell",
    LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP,
    NULL,
    FALSE
    );
  CONST EFI_GUID  FastbootGuid = {
    0xb7c9a614, 0x8a50, 0x4ab8,
    { 0x96,     0x22,   0xba,  0xfd,0x03, 0x6a, 0x41, 0x8e }
  };

  RegisterFvApplication (
    &FastbootGuid,
    L"USB Fastboot",
    LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP,
    &FastbootKey,
    GetBootModeHob () == BOOT_IN_RECOVERY_MODE
    );
  CONST EFI_GUID  BootMenuGuid = {
    0xeec25bdc, 0x67f2, 0x4d95,
    { 0xb1,     0xd5,   0xf8,  0x1b,0x20, 0x39, 0xd1, 0x1d }
  };

  // F12 launches this application; it is not a selectable boot target.
  RegisterFvApplication (
    &BootMenuGuid,
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
