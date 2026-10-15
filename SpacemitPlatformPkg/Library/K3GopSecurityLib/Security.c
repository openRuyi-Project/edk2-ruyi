/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/K3GopPolicyLib.h>
#include <IndustryStandard/PeImage.h>
#include <Protocol/PciIo.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/SecurityManagementLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC EFI_STATUS EFIAPI
CheckImage (
  UINT32                          AuthenticationStatus,
  CONST EFI_DEVICE_PATH_PROTOCOL  *File,
  VOID                            *FileBuffer,
  UINTN                           FileSize,
  BOOLEAN                         BootPolicy
  )
{
  UINT16                          Machine;
  K3_GOP_POLICY                   Policy;
  CONST EFI_DEVICE_PATH_PROTOCOL  *Node;
  EFI_DEVICE_PATH_PROTOCOL        *Remaining;
  EFI_HANDLE                      Controller;
  PCI_TYPE00                      Pci;
  EFI_STATUS                      Status;
  VOID                            *Ready;

  if ((File == NULL) || (FileBuffer == NULL)) {
    return EFI_SUCCESS;
  }

  Policy = K3GopFilePolicy (File);
  if (Policy != K3GopX64) {
    if (EFI_ERROR (gBS->LocateProtocol (&gK3GopPolicyReadyGuid, NULL, &Ready))) {
      return EFI_ACCESS_DENIED;
    }

    return K3GopVerifyImage (Policy, FileBuffer, FileSize) ?
           EFI_SUCCESS : EFI_ACCESS_DENIED;
  }

  if (!IsDevicePathValid (File, 0)) {
    return EFI_ACCESS_DENIED;
  }

  for (Node = File; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if ((DevicePathType (Node) == MEDIA_DEVICE_PATH) &&
        (DevicePathSubType (Node) == MEDIA_RELATIVE_OFFSET_RANGE_DP) &&
        IsDevicePathEnd (NextDevicePathNode (Node)))
    {
      break;
    }
  }

  if (IsDevicePathEnd (Node)) {
    return EFI_SUCCESS;
  }

  if (DevicePathNodeLength (Node) != sizeof (MEDIA_RELATIVE_OFFSET_RANGE_DEVICE_PATH)) {
    return EFI_ACCESS_DENIED;
  }

  Machine = K3GopImageMachine (FileBuffer, FileSize);
  if (Machine == EFI_IMAGE_MACHINE_RISCV64) {
    return EFI_SUCCESS;
  }

  // Foreign images remain blocked until controller binding guards are ready.
  if (EFI_ERROR (gBS->LocateProtocol (&gK3GopPolicyReadyGuid, NULL, &Ready))) {
    return EFI_ACCESS_DENIED;
  }

  // Non-native onboard ROMs default to x64; AMD GCN uses a pinned replacement.
  if (Machine != EFI_IMAGE_MACHINE_X64) {
    return EFI_ACCESS_DENIED;
  }

  Remaining = (EFI_DEVICE_PATH_PROTOCOL *)File;
  Status    = gBS->LocateDevicePath (&gEfiPciIoProtocolGuid, &Remaining, &Controller);
  if (EFI_ERROR (Status) || (Remaining != Node) || EFI_ERROR (K3GopReadPciDevice (Controller, &Pci))) {
    return EFI_ACCESS_DENIED;
  }

  if ((Pci.Hdr.ClassCode[2] == PCI_CLASS_DISPLAY) &&
      (K3GopPolicyForDevice (Pci.Hdr.VendorId, Pci.Hdr.DeviceId) != K3GopX64))
  {
    DEBUG ((DEBUG_INFO, "K3 GOP: skip onboard image for %04x:%04x\n", Pci.Hdr.VendorId, Pci.Hdr.DeviceId));
    return EFI_ACCESS_DENIED;
  }

  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
K3GopSecurityConstructor (
  EFI_HANDLE        ImageHandle,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return RegisterSecurity2Handler (CheckImage, EFI_AUTH_OPERATION_VERIFY_IMAGE | EFI_AUTH_OPERATION_IMAGE_REQUIRED);
}
