/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PciHostBridgeLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/Fu740PciController.h>

PCI_ROOT_BRIDGE *EFIAPI
PciHostBridgeGetRootBridges (
  UINTN  *Count
  )
{
  FU740_PCI_CONTROLLER_PROTOCOL  *Controller;
  PCI_ROOT_BRIDGE                *Bridge;

  if (Count == NULL) {
    return NULL;
  }

  *Count = 0;
  if (EFI_ERROR (
        gBS->LocateProtocol (
               &gFu740PciControllerProtocolGuid,
               NULL,
               (VOID **)&Controller
               )
        ))
  {
    return NULL;
  }

  if (Controller->Revision != 1) {
    return NULL;
  }

  Bridge = AllocateCopyPool (sizeof (*Bridge), &Controller->RootBridge);
  if (Bridge == NULL) {
    return NULL;
  }

  Bridge->DevicePath = DuplicateDevicePath (Controller->RootBridge.DevicePath);
  if (Bridge->DevicePath == NULL) {
    FreePool (Bridge);
    return NULL;
  }

  *Count = 1;
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
  DEBUG ((DEBUG_ERROR, "FU740 PCI resource allocation failed\n"));
}
