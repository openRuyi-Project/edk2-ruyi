/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/PciIo.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/Rtl812xHwLib.h>
#include <Library/Rtl812xSnpLib.h>
#include <Library/SnpAdapterInfoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

typedef struct ADAPTER {
  struct ADAPTER                 *Next;
  EFI_HANDLE                     Controller;
  EFI_HANDLE                     Child;
  EFI_PCI_IO_PROTOCOL            *Pci;
  EFI_SIMPLE_NETWORK_PROTOCOL    *Snp;
  SNP_ADAPTER_INFO               AdapterInfo;
  EFI_DEVICE_PATH_PROTOCOL       *Path;
  EFI_EVENT                      ExitEvent;
  BOOLEAN                        ChildOpened;
  BOOLEAN                        Published;
  BOOLEAN                        Ended;
} ADAPTER;

STATIC ADAPTER  *mAdapters;

STATIC ADAPTER *
Find (
  EFI_HANDLE  Controller
  )
{
  for (ADAPTER *Adapter = mAdapters; Adapter != NULL; Adapter = Adapter->Next) {
    if (Adapter->Controller == Controller) {
      return Adapter;
    }
  }

  return NULL;
}

STATIC BOOLEAN
HasChildResources (
  ADAPTER  *Adapter
  )
{
  return Adapter->Snp || Adapter->Path || Adapter->ExitEvent ||
         Adapter->Child || Adapter->Published || Adapter->ChildOpened;
}

STATIC EFI_STATUS
CheckRemaining (
  EFI_DEVICE_PATH_PROTOCOL  *Remaining
  )
{
  MAC_ADDR_DEVICE_PATH  *Mac;

  if ((Remaining == NULL) || IsDevicePathEnd (Remaining)) {
    return EFI_SUCCESS;
  }

  if ((DevicePathType (Remaining) != MESSAGING_DEVICE_PATH) ||
      (DevicePathSubType (Remaining) != MSG_MAC_ADDR_DP) ||
      (DevicePathNodeLength (Remaining) != sizeof (*Mac)))
  {
    return EFI_UNSUPPORTED;
  }

  Mac = (VOID *)Remaining;
  return Mac->IfType == 1 ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

STATIC EFI_STATUS
MatchChildPath (
  EFI_DEVICE_PATH_PROTOCOL  *Remaining,
  CONST UINT8               Mac[6]
  )
{
  if ((Remaining != NULL) && !IsDevicePathEnd (Remaining)) {
    MAC_ADDR_DEVICE_PATH  *Wanted = (VOID *)Remaining;
    if (CompareMem (Wanted->MacAddress.Addr, Mac, 6)) {
      return EFI_NOT_FOUND;
    }
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
GetChildMac (
  EFI_PCI_IO_PROTOCOL *Pci,
  EFI_DEVICE_PATH_PROTOCOL  *Remaining,
  UINT8                     Mac[6]
  )
{
  EFI_STATUS  Status = RtlReadStationAddress (Pci, Mac);

  return EFI_ERROR (Status) ? Status : MatchChildPath (Remaining, Mac);
}

STATIC EFI_STATUS
CheckController (
  EFI_PCI_IO_PROTOCOL  *Pci
  )
{
  UINT32      Id;
  EFI_STATUS  Status = Pci->Pci.Read (Pci, EfiPciIoWidthUint32, 0, 1, &Id);

  if (!EFI_ERROR (Status) && (Id != gRtlChip.PciId)) {
    Status = EFI_UNSUPPORTED;
  }

  return Status;
}

STATIC EFI_STATUS EFIAPI
Supported (
  EFI_DRIVER_BINDING_PROTOCOL  *This,
  EFI_HANDLE                   Controller,
  EFI_DEVICE_PATH_PROTOCOL     *Remaining
  )
{
  ADAPTER              *Adapter;
  EFI_PCI_IO_PROTOCOL  *Pci;
  EFI_STATUS           Status;
  EFI_STATUS           Cleanup;
  UINT8                Mac[6];

  Status = CheckRemaining (Remaining);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Adapter = Find (Controller);
  if (Adapter != NULL) {
    if (Adapter->Ended) {
      return EFI_DEVICE_ERROR;
    }

    if ((Remaining != NULL) && IsDevicePathEnd (Remaining)) {
      return EFI_ALREADY_STARTED;
    }

    if (Adapter->Published && Adapter->ChildOpened) {
      Status = MatchChildPath (Remaining, Adapter->Snp->Mode->PermanentAddress.Addr);
      return EFI_ERROR (Status) ? Status : EFI_ALREADY_STARTED;
    }

    if (HasChildResources (Adapter)) {
      return EFI_NOT_READY;
    }
  }

  if (Adapter != NULL) {
    return (Remaining && !IsDevicePathEnd (Remaining)) ?
           GetChildMac (Adapter->Pci, Remaining, Mac) : EFI_SUCCESS;
  }

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&Pci,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status  = CheckController (Pci);
  if (!EFI_ERROR (Status) && Remaining && !IsDevicePathEnd (Remaining)) {
    Status = GetChildMac (Pci, Remaining, Mac);
  }
  Cleanup = gBS->CloseProtocol (
                   Controller,
                   &gEfiPciIoProtocolGuid,
                   This->DriverBindingHandle,
                   Controller
                   );
  return EFI_ERROR (Status) ? Status : Cleanup;
}

STATIC VOID EFIAPI
Exit (
  EFI_EVENT  Event,
  VOID       *Context
  )
{
  ADAPTER     *Adapter = Context;
  EFI_STATUS  Status;

  Adapter->Ended = TRUE;
  Status         = Rtl812xSnpExit (Adapter->Snp);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RTL%04x SNP DMA stop failed: %r; ownership retained\n", gRtlChip.PciId >> 16, Status));
  }
}

STATIC EFI_STATUS
RemoveChild (
  EFI_DRIVER_BINDING_PROTOCOL  *This,
  ADAPTER                      *Adapter
  )
{
  EFI_STATUS  Status;

  if (Adapter->Ended) {
    return EFI_DEVICE_ERROR;
  }

  if (Adapter->Snp && (Adapter->Snp->Mode->State == EfiSimpleNetworkInitialized)) {
    Status = Adapter->Snp->Shutdown (Adapter->Snp);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  if (Adapter->Published) {
    Status = gBS->UninstallMultipleProtocolInterfaces (
                    Adapter->Child,
                    &gEfiSimpleNetworkProtocolGuid,
                    Adapter->Snp,
                    &gEfiAdapterInformationProtocolGuid,
                    &Adapter->AdapterInfo.Protocol,
                    &gEfiDevicePathProtocolGuid,
                    Adapter->Path,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Adapter->Published = FALSE;
  }

  if (Adapter->ChildOpened) {
    Status = gBS->CloseProtocol (
                    Adapter->Controller,
                    &gEfiPciIoProtocolGuid,
                    This->DriverBindingHandle,
                    Adapter->Child
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Adapter->ChildOpened = FALSE;
  }

  Status = Rtl812xSnpDestroy (&Adapter->Snp);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Adapter->AdapterInfo, sizeof (Adapter->AdapterInfo));
  if (Adapter->ExitEvent != NULL) {
    Status = gBS->CloseEvent (Adapter->ExitEvent);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Adapter->ExitEvent = NULL;
  }

  if (Adapter->Path != NULL) {
    FreePool (Adapter->Path);
    Adapter->Path = NULL;
  }

  Adapter->Child = NULL;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
RemoveController (
  EFI_DRIVER_BINDING_PROTOCOL  *This,
  ADAPTER                      *Adapter
  )
{
  ADAPTER     **Link;
  EFI_STATUS  Status = RemoveChild (This, Adapter);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CloseProtocol (
                  Adapter->Controller,
                  &gEfiPciIoProtocolGuid,
                  This->DriverBindingHandle,
                  Adapter->Controller
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Link = &mAdapters;
  while (*Link != Adapter) {
    Link = &(*Link)->Next;
  }

  *Link = Adapter->Next;
  FreePool (Adapter);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
CreateChild (
  EFI_DRIVER_BINDING_PROTOCOL  *This,
  ADAPTER                      *Adapter,
  EFI_DEVICE_PATH_PROTOCOL     *Remaining
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *ParentPath;
  MAC_ADDR_DEVICE_PATH      Node;
  EFI_STATUS                Status;
  UINT8                     Mac[6];

  Status = CheckController (Adapter->Pci);
  if (!EFI_ERROR (Status)) {
    Status = GetChildMac (Adapter->Pci, Remaining, Mac);
  }

  if (!EFI_ERROR (Status)) {
    Status = Rtl812xSnpCreate (Adapter->Pci, Mac, &Adapter->Snp);
  }

  if (!EFI_ERROR (Status)) {
    Status = SnpAdapterInfoInitialize (&Adapter->AdapterInfo, Adapter->Snp);
  }

  if (!EFI_ERROR (Status)) {
    ParentPath = DevicePathFromHandle (Adapter->Controller);
    ZeroMem (&Node, sizeof (Node));
    Node.Header.Type    = MESSAGING_DEVICE_PATH;
    Node.Header.SubType = MSG_MAC_ADDR_DP;
    SetDevicePathNodeLength (&Node.Header, sizeof (Node));
    Node.IfType = 1;
    CopyMem (Node.MacAddress.Addr, Mac, 6);
    Adapter->Path = ParentPath ? AppendDevicePathNode (ParentPath, &Node.Header) : NULL;
    if (Adapter->Path == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
    }
  }

  if (!EFI_ERROR (Status)) {
    Status = gBS->CreateEventEx (
                    EVT_NOTIFY_SIGNAL,
                    TPL_NOTIFY,
                    Exit,
                    Adapter,
                    &gEfiEventBeforeExitBootServicesGuid,
                    &Adapter->ExitEvent
                    );
  }

  if (!EFI_ERROR (Status)) {
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &Adapter->Child,
                    &gEfiSimpleNetworkProtocolGuid,
                    Adapter->Snp,
                    &gEfiAdapterInformationProtocolGuid,
                    &Adapter->AdapterInfo.Protocol,
                    &gEfiDevicePathProtocolGuid,
                    Adapter->Path,
                    NULL
                    );
    Adapter->Published = !EFI_ERROR (Status);
  }

  if (!EFI_ERROR (Status)) {
    Status = gBS->OpenProtocol (
                    Adapter->Controller,
                    &gEfiPciIoProtocolGuid,
                    (VOID **)&Adapter->Pci,
                    This->DriverBindingHandle,
                    Adapter->Child,
                    EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
                    );
    Adapter->ChildOpened = !EFI_ERROR (Status);
  }

  if (!EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "RTL%04x SNP published: %02x:%02x:%02x:%02x:%02x:%02x\n",
      gRtlChip.PciId >> 16,
      Mac[0],
      Mac[1],
      Mac[2],
      Mac[3],
      Mac[4],
      Mac[5]
      ));
  }

  return Status;
}

STATIC EFI_STATUS EFIAPI
Start (
  EFI_DRIVER_BINDING_PROTOCOL  *This,
  EFI_HANDLE                   Controller,
  EFI_DEVICE_PATH_PROTOCOL     *Remaining
  )
{
  ADAPTER     *Adapter      = Find (Controller);
  BOOLEAN     NewController = Adapter == NULL;
  EFI_STATUS  Status;
  EFI_STATUS  Cleanup;

  Status = CheckRemaining (Remaining);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (NewController) {
    Adapter = AllocateZeroPool (sizeof (*Adapter));
    if (Adapter == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    Adapter->Controller = Controller;
    Status              = gBS->OpenProtocol (
                                 Controller,
                                 &gEfiPciIoProtocolGuid,
                                 (VOID **)&Adapter->Pci,
                                 This->DriverBindingHandle,
                                 Controller,
                                 EFI_OPEN_PROTOCOL_BY_DRIVER
                                 );
    if (EFI_ERROR (Status)) {
      FreePool (Adapter);
      return Status;
    }

    Adapter->Next = mAdapters;
    mAdapters     = Adapter;
  } else if (Adapter->Ended) {
    return EFI_DEVICE_ERROR;
  }

  if ((Remaining != NULL) && IsDevicePathEnd (Remaining)) {
    Status = NewController ? CheckController (Adapter->Pci) : EFI_SUCCESS;
  } else if (HasChildResources (Adapter)) {
    if (Adapter->Published && Adapter->ChildOpened) {
      Status = MatchChildPath (Remaining, Adapter->Snp->Mode->PermanentAddress.Addr);
      return EFI_ERROR (Status) ? Status : EFI_ALREADY_STARTED;
    }

    return EFI_NOT_READY;
  } else {
    Status = CreateChild (This, Adapter, Remaining);
  }

  if (EFI_ERROR (Status)) {
    Cleanup = NewController ? RemoveController (This, Adapter) : RemoveChild (This, Adapter);
    if (EFI_ERROR (Cleanup)) {
      DEBUG ((DEBUG_ERROR, "RTL%04x SNP startup cleanup failed: %r; owner retained\n", gRtlChip.PciId >> 16, Cleanup));
      return Cleanup;
    }
  }

  return Status;
}

STATIC EFI_STATUS EFIAPI
Stop (
  EFI_DRIVER_BINDING_PROTOCOL  *This,
  EFI_HANDLE                   Controller,
  UINTN                        Children,
  EFI_HANDLE                   *ChildBuffer
  )
{
  ADAPTER  *Adapter = Find (Controller);

  if (Adapter == NULL) {
    return EFI_NOT_STARTED;
  }

  if (Children != 0) {
    if ((Children != 1) || (ChildBuffer == NULL) || (ChildBuffer[0] != Adapter->Child)) {
      return EFI_INVALID_PARAMETER;
    }

    return RemoveChild (This, Adapter);
  }

  if (Adapter->ChildOpened) {
    return EFI_DEVICE_ERROR;
  }

  // Failed startup can leave published interfaces without a child relation.
  // The parent's Stop must be able to retry that partial cleanup.
  return RemoveController (This, Adapter);
}

STATIC EFI_DRIVER_BINDING_PROTOCOL  mBinding = {
  Supported, Start, Stop, 0x10, NULL, NULL
};

EFI_STATUS EFIAPI
Rtl812xSnpEntry (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBindingComponentName2 (
           Image,
           SystemTable,
           &mBinding,
           Image,
           NULL,
           NULL
           );
}
