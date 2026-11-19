/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PciHostBridgeLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/PlatformPciController.h>

PCI_ROOT_BRIDGE *EFIAPI
PciHostBridgeGetRootBridges (
  UINTN  *Count
  )
{
  RUYI_PCI_CONTROLLER_PROTOCOL  *Controller;
  PCI_ROOT_BRIDGE                *Bridge;

  if (Count == NULL) {
    return NULL;
  }

  *Count = 0;
  if (EFI_ERROR (
        gBS->LocateProtocol (
               &gRuyiPciControllerProtocolGuid,
               NULL,
               (VOID **)&Controller
               )
        ))
  {
    return NULL;
  }

  UINTN Entries;
  CONST PCI_ROOT_BRIDGE *Roots = PlatformPciRoots (Controller, &Entries);
  if (!Roots || !Entries || Entries > MAX_UINTN / sizeof (*Bridge)) {
    return NULL;
  }
  Bridge = AllocateCopyPool (Entries * sizeof (*Bridge), Roots);
  if (Bridge == NULL) {
    return NULL;
  }
  for (UINTN Index = 0; Index < Entries; Index++) {
    Bridge[Index].DevicePath = DuplicateDevicePath (Roots[Index].DevicePath);
    if (!Bridge[Index].DevicePath) {
      PciHostBridgeFreeRootBridges (Bridge, Index);
      return NULL;
    }
  }
  *Count = Entries;
  return Bridge;
}

VOID EFIAPI
PciHostBridgeFreeRootBridges (
  PCI_ROOT_BRIDGE  *Bridges,
  UINTN            Count
  )
{
  if (Bridges != NULL) {
    for (UINTN Index = 0; Index < Count; Index++) {
      FreePool (Bridges[Index].DevicePath);
    }

    FreePool (Bridges);
  }
}

VOID EFIAPI
PciHostBridgeResourceConflict (
  EFI_HANDLE  Host,
  VOID        *Configuration
  )
{
  DEBUG ((DEBUG_ERROR, "PCI resource allocation failed\n"));
}
