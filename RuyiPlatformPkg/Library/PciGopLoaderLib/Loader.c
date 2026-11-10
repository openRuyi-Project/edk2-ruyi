/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/PciGopLoaderLib.h>
#include <Library/PciGopPolicyLib.h>
#include <IndustryStandard/PeImage.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/PciIo.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

extern EFI_GUID  gMultiArchUefiEmulatorDeviceGuid;

VOID
PciGopConnectEmulator (
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
  if (Status == EFI_NOT_FOUND) {
    DEBUG ((DEBUG_INFO, "UEFI driver emulation disabled\n"));
    return;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UEFI driver emulator unavailable: %r\n", Status));
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
      DEBUG ((DEBUG_ERROR, "UEFI driver emulator start failed: %r\n", Status));
    }
  }

  FreePool (Handles);
}

typedef struct GOP_BINDING {
  struct GOP_BINDING              *Next;
  EFI_DRIVER_BINDING_PROTOCOL     *Binding;
  EFI_HANDLE                      Image;
  EFI_DRIVER_BINDING_SUPPORTED    Supported;
  EFI_DRIVER_BINDING_START        Start;
  UINT16                          Machine;
  PCI_GOP_POLICY                  Replacement;
} GOP_BINDING;

STATIC GOP_BINDING  *mBindings;
STATIC EFI_EVENT    mBindingEvent;
STATIC EFI_HANDLE   mPolicyHandle;
STATIC VOID         *mRegistration;
STATIC BOOLEAN      mEnabled;
STATIC EFI_HANDLE   mGopImage;
STATIC BOOLEAN      mGopAttempted;

STATIC GOP_BINDING *
FindBinding (
  EFI_DRIVER_BINDING_PROTOCOL  *This
  )
{
  for (GOP_BINDING *Item = mBindings; Item != NULL; Item = Item->Next) {
    if (Item->Binding == This) {
      return Item;
    }
  }

  return NULL;
}

STATIC BOOLEAN
BindingAllowed (
  GOP_BINDING  *Item,
  EFI_HANDLE   Controller
  )
{
  PCI_TYPE00                 Pci;
  EFI_PCI_IO_PROTOCOL        *Io;
  EFI_LOADED_IMAGE_PROTOCOL  *Image;
  EFI_STATUS                 Status;
  PCI_GOP_POLICY             Policy;

  if ((Item == NULL) || !mEnabled ||
      EFI_ERROR (gBS->HandleProtocol (Item->Image, &gEfiLoadedImageProtocolGuid, (VOID **)&Image)))
  {
    return FALSE;
  }

  Status = gBS->HandleProtocol (Controller, &gEfiPciIoProtocolGuid, (VOID **)&Io);
  if (EFI_ERROR (Status)) {
    return Item->Replacement == PciGopX64;
  }

  Status = PciGopReadPciDevice (Controller, &Pci);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  if (Pci.Hdr.ClassCode[2] != PCI_CLASS_DISPLAY) {
    return Item->Replacement == PciGopX64;
  }

  Policy = PciGopPolicyForDevice (Pci.Hdr.VendorId, Pci.Hdr.DeviceId);
  if (Item->Machine == EFI_IMAGE_MACHINE_X64) {
    return Policy == PciGopX64;
  }

  return (Policy != PciGopX64) && (Policy == Item->Replacement) &&
         (mGopImage == Item->Image);
}

STATIC VOID
ReportBindingFailure (
  CONST CHAR8  *Operation,
  GOP_BINDING  *Item,
  EFI_HANDLE   Controller,
  BOOLEAN      Allowed,
  EFI_STATUS   Status
  )
{
  PCI_TYPE00 Pci;

  if (EFI_ERROR (Status) &&
      !EFI_ERROR (PciGopReadPciDevice (Controller, &Pci)) &&
      Pci.Hdr.ClassCode[2] == PCI_CLASS_DISPLAY) {
    DEBUG ((DEBUG_INFO, "PCI GOP %04x:%04x %a: %r (policy %a, machine %04x)\n",
            Pci.Hdr.VendorId, Pci.Hdr.DeviceId, Operation, Status,
            Allowed ? "allowed" : "blocked", Item ? Item->Machine : 0));
  }
}

STATIC EFI_STATUS EFIAPI
GuardSupported (
  EFI_DRIVER_BINDING_PROTOCOL  *This,
  EFI_HANDLE                   Controller,
  EFI_DEVICE_PATH_PROTOCOL     *Remaining
  )
{
  GOP_BINDING  *Item = FindBinding (This);
  BOOLEAN      Allowed = BindingAllowed (Item, Controller);
  EFI_STATUS   Status = Allowed ? Item->Supported (This, Controller, Remaining) : EFI_UNSUPPORTED;

  ReportBindingFailure ("Supported", Item, Controller, Allowed, Status);
  return Status;
}

STATIC EFI_STATUS EFIAPI
GuardStart (
  EFI_DRIVER_BINDING_PROTOCOL  *This,
  EFI_HANDLE                   Controller,
  EFI_DEVICE_PATH_PROTOCOL     *Remaining
  )
{
  GOP_BINDING  *Item = FindBinding (This);
  BOOLEAN      Allowed = BindingAllowed (Item, Controller);
  EFI_STATUS   Status = Allowed ? Item->Start (This, Controller, Remaining) : EFI_UNSUPPORTED;

  ReportBindingFailure ("Start", Item, Controller, Allowed, Status);
  return Status;
}

STATIC EFI_STATUS EFIAPI
RejectBinding (
  EFI_DRIVER_BINDING_PROTOCOL  *This,
  EFI_HANDLE                   Controller,
  EFI_DEVICE_PATH_PROTOCOL     *Remaining
  )
{
  return EFI_UNSUPPORTED;
}

STATIC VOID EFIAPI
BindingInstalled (
  EFI_EVENT  Event,
  VOID       *Context
  )
{
  EFI_HANDLE                   Handle;
  EFI_DRIVER_BINDING_PROTOCOL  *Binding;
  EFI_LOADED_IMAGE_PROTOCOL    *Image;
  GOP_BINDING                  *Item;
  EFI_STATUS                   Status;
  UINTN                        Size;
  UINT16                       Machine;

  for ( ; ;) {
    Size   = sizeof (Handle);
    Status = gBS->LocateHandle (ByRegisterNotify, NULL, mRegistration, &Size, &Handle);
    if (EFI_ERROR (Status)) {
      return;
    }

    if (EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiDriverBindingProtocolGuid, (VOID **)&Binding)) ||
        EFI_ERROR (gBS->HandleProtocol (Binding->ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Image)))
    {
      continue;
    }

    Machine = PciGopImageMachine (Image->ImageBase, (UINTN)Image->ImageSize);
    if ((Machine != EFI_IMAGE_MACHINE_X64) && (Machine != EFI_IMAGE_MACHINE_AARCH64)) {
      continue;
    }

    if ((Binding->Supported == GuardSupported) && (Binding->Start == GuardStart)) {
      continue;
    }

    Item = FindBinding (Binding);
    if (Item == NULL) {
      Item = AllocateZeroPool (sizeof (*Item));
      if (Item == NULL) {
        Binding->Supported = RejectBinding;
        Binding->Start     = RejectBinding;
        DEBUG ((DEBUG_ERROR, "PCI GOP: cannot register driver binding policy\n"));
        continue;
      }

      Item->Next = mBindings;
      mBindings  = Item;
    }

    Item->Binding = Binding;
    Item->Image   = Binding->ImageHandle;
    if (Binding->Supported != GuardSupported) {
      Item->Supported = Binding->Supported;
    }

    if (Binding->Start != GuardStart) {
      Item->Start = Binding->Start;
    }

    Item->Machine     = Machine;
    Item->Replacement = PciGopX64;
    if (Item->Image == mGopImage) {
      Item->Replacement = PciGopAmdPreSoc15;
    }

    // Guard binding as well as loading: a driver may support other PCI devices.
    Binding->Supported = GuardSupported;
    Binding->Start     = GuardStart;
  }
}

EFI_STATUS
PciGopInitialize (
  VOID
  )
{
  EFI_STATUS  Status;

  mEnabled = PcdGetBool (PcdMauEmulationEnabled);
  if (!mEnabled || (mBindingEvent != NULL)) {
    return EFI_SUCCESS;
  }

  Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, BindingInstalled, NULL, &mBindingEvent);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->RegisterProtocolNotify (&gEfiDriverBindingProtocolGuid, mBindingEvent, &mRegistration);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mBindingEvent);
    mBindingEvent = NULL;
    return Status;
  }

  Status = gBS->SignalEvent (mBindingEvent);
  if (!EFI_ERROR (Status)) {
    Status = gBS->InstallProtocolInterface (&mPolicyHandle, &gPciGopPolicyReadyGuid, EFI_NATIVE_INTERFACE, NULL);
  }

  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mBindingEvent);
    mBindingEvent = NULL;
    mEnabled      = FALSE;
  }

  return Status;
}

STATIC EFI_STATUS
LoadReplacement (
  VOID
  )
{
  EFI_STATUS                         Status;
  VOID                               *Buffer = NULL;
  UINTN                              Size;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  File;
  EFI_DEVICE_PATH_PROTOCOL           *Path;

  if (mGopAttempted) {
    return mGopImage != NULL ? EFI_SUCCESS : EFI_NOT_FOUND;
  }

  mGopAttempted = TRUE;
  Status        = GetSectionFromAnyFv (PciGopFileGuid (PciGopAmdPreSoc15), EFI_SECTION_RAW, 0, &Buffer, &Size);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!PciGopVerifyImage (PciGopAmdPreSoc15, Buffer, Size)) {
    FreePool (Buffer);
    return EFI_SECURITY_VIOLATION;
  }

  EfiInitializeFwVolDevicepathNode (&File, PciGopFileGuid (PciGopAmdPreSoc15));
  Path = AppendDevicePathNode (NULL, &File.Header);
  if (Path == NULL) {
    FreePool (Buffer);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->LoadImage (FALSE, gImageHandle, Path, Buffer, Size, &mGopImage);
  FreePool (Path);
  FreePool (Buffer);
  if (!EFI_ERROR (Status)) {
    // Publish the image handle before StartImage registers its binding.
    Status = gBS->StartImage (mGopImage, NULL, NULL);
  }

  if (EFI_ERROR (Status)) {
    mGopImage = NULL;
  }

  return Status;
}

VOID
PciGopConnectSelectedDrivers (
  VOID
  )
{
  EFI_HANDLE      *Handles;
  EFI_HANDLE      Drivers[2];
  UINTN           Count;
  PCI_TYPE00      Pci;
  EFI_STATUS      Status;
  PCI_GOP_POLICY  Policy;

  if (!mEnabled || (mBindingEvent == NULL)) {
    return;
  }

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return;
  }

  for (UINTN Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (PciGopReadPciDevice (Handles[Index], &Pci)) ||
        (Pci.Hdr.ClassCode[2] != PCI_CLASS_DISPLAY))
    {
      continue;
    }

    Policy = PciGopPolicyForDevice (Pci.Hdr.VendorId, Pci.Hdr.DeviceId);
    if (Policy == PciGopX64) {
      continue;
    }

    Status = LoadReplacement ();
    if (!EFI_ERROR (Status)) {
      Drivers[0] = mGopImage;
      Drivers[1] = NULL;
      Status     = gBS->ConnectController (Handles[Index], Drivers, NULL, TRUE);
    }

    DEBUG ((
      DEBUG_INFO,
      "PCI GOP: %04x:%04x AMD ARM64 1.68: %r\n",
      Pci.Hdr.VendorId,
      Pci.Hdr.DeviceId,
      Status
      ));
  }

  FreePool (Handles);
}
