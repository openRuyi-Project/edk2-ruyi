/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Guid/Fdt.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/PlatformMacAddressLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC VOID EFIAPI
Publish (EFI_EVENT Event, VOID *Context)
{
  EFI_STATUS Status;
  EFI_PHYSICAL_ADDRESS Address = 0;
  VOID *Original, *Copy;
  UINTN Pages, Index;
  INT32 Aliases, Node, Length;
  CONST CHAR8 *Path;
  CHAR8 Name[16];
  UINT8 Mac[6];

  gBS->CloseEvent (Event);
  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Original);
  if (EFI_ERROR (Status) || FdtCheckHeader (Original) || FdtTotalSize (Original) > SIZE_1MB) {
    DEBUG ((DEBUG_ERROR, "P550 OS device tree unavailable\n"));
    return;
  }
  Pages = EFI_SIZE_TO_PAGES (FdtTotalSize (Original) + EFI_PAGE_SIZE);
  Status = gBS->AllocatePages (AllocateAnyPages, EfiACPIReclaimMemory, Pages, &Address);
  if (EFI_ERROR (Status)) { return; }
  Copy = (VOID *)(UINTN)Address;
  if (FdtOpenInto (Original, Copy, EFI_PAGES_TO_SIZE (Pages))) { goto Failed; }
  for (Index = 0; Index < 2; Index++) {
    Status = PlatformGetMacAddress (Index, Mac);
    if (EFI_ERROR (Status)) { goto Failed; }
    // Property insertion can change offsets. Resolve aliases again each time.
    Aliases = FdtPathOffset (Copy, "/aliases");
    AsciiSPrint (Name, sizeof (Name), "ethernet%u", Index);
    Path = FdtGetProp (Copy, Aliases, Name, &Length);
    if (!Path || Length < 2 || Path[Length - 1]) { goto Failed; }
    Node = FdtPathOffset (Copy, Path);
    if (Node < 0 || FdtSetProp (Copy, Node, "local-mac-address", Mac, sizeof (Mac)) ||
        FdtSetProp (Copy, Node, "mac-address", Mac, sizeof (Mac))) { goto Failed; }
  }
  Status = gBS->InstallConfigurationTable (&gFdtTableGuid, Copy);
  if (EFI_ERROR (Status)) { goto Failed; }
  DEBUG ((DEBUG_INFO, "P550 OS device tree preserves factory Ethernet addresses\n"));
  return;
Failed:
  DEBUG ((DEBUG_ERROR, "P550 OS device tree update failed\n"));
  gBS->FreePages (Address, Pages);
}

EFI_STATUS EFIAPI
PremierP550FdtEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_EVENT Event;
  return gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Publish, NULL,
                            &gEfiEventReadyToBootGuid, &Event);
}
