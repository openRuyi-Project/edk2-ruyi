/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Protocol/PlatformBootManager.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UfsStorageLib.h>

STATIC UFS_DEVICE_PATH *
UfsNode (
  EFI_DEVICE_PATH_PROTOCOL  *Path
  )
{
  if (!Path || !IsDevicePathValid (Path, 0)) {
    return NULL;
  }

  for (EFI_DEVICE_PATH_PROTOCOL *Node = Path; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if ((DevicePathType (Node) == MESSAGING_DEVICE_PATH) && (DevicePathSubType (Node) == MSG_UFS_DP) &&
        (DevicePathNodeLength (Node) == sizeof (UFS_DEVICE_PATH)))
    {
      return (VOID *)Node;
    }
  }

  return NULL;
}

STATIC EFI_STATUS EFIAPI
RefreshStorageOptions (
  CONST EFI_BOOT_MANAGER_LOAD_OPTION  *Options,
  CONST UINTN                         Count,
  EFI_BOOT_MANAGER_LOAD_OPTION        **Updated,
  UINTN                               *UpdatedCount
  )
{
  UFS_USER_MEDIA                Main;
  UFS_DEVICE_PATH               *MainNode;
  EFI_BOOT_MANAGER_LOAD_OPTION  *List;
  UINTN                         Used = 0;

  if (!Updated || !UpdatedCount || (!Options && Count)) {
    return EFI_INVALID_PARAMETER;
  }

  *Updated      = NULL;
  *UpdatedCount = 0;
  if (!Count) {
    return EFI_SUCCESS;
  }

  if (Count > MAX_UINTN / sizeof (*List)) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (EFI_ERROR (UfsFindUserMedia (&Main))) {
    ZeroMem (&Main, sizeof (Main));
  }

  MainNode = UfsNode (Main.Path);
  List     = AllocateZeroPool (Count * sizeof (*List));
  if (!List) {
    return EFI_OUT_OF_RESOURCES;
  }

  for (UINTN I = 0; I < Count; I++) {
    UFS_DEVICE_PATH  *Node = UfsNode (Options[I].FilePath);
    if (Node) {
      // RPMB and other well-known LUNs are not ordinary boot disks.
      if (Node->Lun & 0x80) {
        continue;
      }

      if (MainNode) {
        UINTN  Prefix     = (UINT8 *)Node - (UINT8 *)Options[I].FilePath;
        UINTN  MainPrefix = (UINT8 *)MainNode - (UINT8 *)Main.Path;
        if ((Prefix == MainPrefix) && (Node->Pun == MainNode->Pun) &&
            (CompareMem (Options[I].FilePath, Main.Path, Prefix) == 0))
        {
          if (Node->Lun != MainNode->Lun) {
            continue;
          }
        }
      }
    }

    EFI_STATUS  Status = EfiBootManagerInitializeLoadOption (
                           &List[Used],
                           Options[I].OptionNumber,
                           Options[I].OptionType,
                           Options[I].Attributes,
                           Options[I].Description,
                           Options[I].FilePath,
                           Options[I].OptionalData,
                           Options[I].OptionalDataSize
                           );
    if (EFI_ERROR (Status)) {
      EfiBootManagerFreeLoadOptions (List, Used);
      return Status;
    }

    Used++;
  }

  *Updated      = List;
  *UpdatedCount = Used;
  return EFI_SUCCESS;
}

EFI_STATUS
InstallStorageBootPolicy (
  VOID
  )
{
  STATIC EFI_HANDLE                            Handle;
  STATIC EDKII_PLATFORM_BOOT_MANAGER_PROTOCOL  Protocol = {
    EDKII_PLATFORM_BOOT_MANAGER_PROTOCOL_REVISION, RefreshStorageOptions
  };

  if (Handle) {
    return EFI_SUCCESS;
  }

  return gBS->InstallProtocolInterface (
                &Handle,
                &gEdkiiPlatformBootManagerProtocolGuid,
                EFI_NATIVE_INTERFACE,
                &Protocol
                );
}
