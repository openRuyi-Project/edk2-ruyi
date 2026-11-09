/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <IndustryStandard/Emmc.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/PlatformBootManager.h>

STATIC BOOLEAN
IsEmmcBootPartition (
  CONST EFI_DEVICE_PATH_PROTOCOL  *Path
  )
{
  CONST CONTROLLER_DEVICE_PATH  *Controller;
  UINT32                       Number;

  if (Path == NULL) {
    return FALSE;
  }

  while (!IsDevicePathEnd (Path)) {
    if ((DevicePathType (Path) == MESSAGING_DEVICE_PATH) &&
        (DevicePathSubType (Path) == MSG_EMMC_DP))
    {
      Path = NextDevicePathNode (Path);
      if ((DevicePathType (Path) == HARDWARE_DEVICE_PATH) &&
          (DevicePathSubType (Path) == HW_CONTROLLER_DP) &&
          (DevicePathNodeLength (Path) == sizeof (*Controller)))
      {
        Controller = (CONST CONTROLLER_DEVICE_PATH *)Path;
        Number     = ReadUnaligned32 (&Controller->ControllerNumber);
        return (Number == EmmcPartitionBoot1) || (Number == EmmcPartitionBoot2);
      }

      return FALSE;
    }

    Path = NextDevicePathNode (Path);
  }

  return FALSE;
}

STATIC EFI_STATUS EFIAPI
RefreshBootOptions (
  CONST EFI_BOOT_MANAGER_LOAD_OPTION  *Options,
  CONST UINTN                        Count,
  EFI_BOOT_MANAGER_LOAD_OPTION        **Updated,
  UINTN                              *UpdatedCount
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  *Filtered;
  EFI_STATUS                   Status;
  UINTN                        Index;
  UINTN                        Kept;

  if ((Updated == NULL) || (UpdatedCount == NULL) ||
      ((Count != 0) && (Options == NULL)) ||
      (Count > MAX_UINTN / sizeof (*Filtered)))
  {
    return EFI_INVALID_PARAMETER;
  }

  *Updated      = NULL;
  *UpdatedCount = 0;
  if (Count == 0) {
    return EFI_SUCCESS;
  }

  Filtered = AllocateZeroPool (Count * sizeof (*Filtered));
  if (Filtered == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  for (Index = 0, Kept = 0; Index < Count; Index++) {
    // Keep the raw partitions accessible. Only suppress automatic boot entries.
    if (IsEmmcBootPartition (Options[Index].FilePath)) {
      continue;
    }

    Status = EfiBootManagerInitializeLoadOption (
               &Filtered[Kept],
               Options[Index].OptionNumber,
               Options[Index].OptionType,
               Options[Index].Attributes,
               Options[Index].Description,
               Options[Index].FilePath,
               Options[Index].OptionalData,
               Options[Index].OptionalDataSize
               );
    if (EFI_ERROR (Status)) {
      EfiBootManagerFreeLoadOptions (Filtered, Kept);
      return Status;
    }

    Kept++;
  }

  *Updated      = Filtered;
  *UpdatedCount = Kept;
  return EFI_SUCCESS;
}

VOID
RegisterBootOptionsPolicy (
  VOID
  )
{
  STATIC EDKII_PLATFORM_BOOT_MANAGER_PROTOCOL  Policy = {
    EDKII_PLATFORM_BOOT_MANAGER_PROTOCOL_REVISION,
    RefreshBootOptions
  };
  EFI_HANDLE  Handle = NULL;
  EFI_STATUS  Status;

  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &gEdkiiPlatformBootManagerProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &Policy
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Boot option policy: %r\n", Status));
  }
}
