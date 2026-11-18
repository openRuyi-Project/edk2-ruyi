/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Guid/Fdt.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Guid/PremierP550CpuConfig.h>
#include <Library/CpuConfigLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/PlatformMacAddressLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC EFI_STATUS
AddP550CpuOpp (VOID *Fdt)
{
  CPU_CONFIG Config;
  EFI_STATUS Status;
  CONST UINT32 *Phandle;
  CONST UINT64 *Hz;
  INT32 Cpu, Table, Node, Length;
  UINT32 Voltage = CpuToFdt32 (900000);
  UINT32 Latency = CpuToFdt32 (70000);

  if (FdtStringListSearch (Fdt, 0, "compatible", "sifive,hifive-premier-p550") < 0) {
    return EFI_SUCCESS;
  }
  Status = CpuConfigGet (&Config);
  if (EFI_ERROR (Status) || Config.Default != P550_CPU_ALLOW_1600) { return Status; }

  Cpu = FdtPathOffset (Fdt, "/cpus/cpu@0");
  Phandle = FdtGetProp (Fdt, Cpu, "operating-points-v2", &Length);
  if (Phandle == NULL || Length != sizeof (*Phandle)) { return EFI_COMPROMISED_DATA; }
  Table = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Phandle));
  if (Table < 0 || FdtStringListSearch (Fdt, Table, "compatible", "operating-points-v2") < 0) {
    return EFI_COMPROMISED_DATA;
  }
  for (Node = FdtFirstSubnode (Fdt, Table); Node >= 0; Node = FdtNextSubnode (Fdt, Node)) {
    Hz = FdtGetProp (Fdt, Node, "opp-hz", &Length);
    if ((Hz != NULL) && (Length == sizeof (*Hz)) &&
        (Fdt64ToCpu (ReadUnaligned64 (Hz)) == 1600000000)) {
      // Preserve any restrictions supplied by the board device tree.
      return EFI_SUCCESS;
    }
  }
  Node = FdtAddSubnode (Fdt, Table, "opp-1600000000");
  if (Node < 0) { return EFI_DEVICE_ERROR; }
  if (FdtSetPropU64 (Fdt, Node, "opp-hz", 1600000000) ||
      FdtSetProp (Fdt, Node, "opp-microvolt", &Voltage, sizeof (Voltage)) ||
      FdtSetProp (Fdt, Node, "clock-latency-ns", &Latency, sizeof (Latency))) {
    FdtDelNode (Fdt, Node);
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

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
    DEBUG ((DEBUG_ERROR, "EIC7700 OS device tree unavailable\n"));
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
  Status = AddP550CpuOpp (Copy);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "P550 CPU operating point unavailable: %r\n", Status));
  }
  Status = gBS->InstallConfigurationTable (&gFdtTableGuid, Copy);
  if (EFI_ERROR (Status)) { goto Failed; }
  DEBUG ((DEBUG_INFO, "EIC7700 OS device tree publishes Ethernet addresses\n"));
  return;
Failed:
  DEBUG ((DEBUG_ERROR, "EIC7700 OS device tree update failed\n"));
  gBS->FreePages (Address, Pages);
}

EFI_STATUS EFIAPI
Eic7700FdtEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_EVENT Event;
  return gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Publish, NULL,
                            &gEfiEventReadyToBootGuid, &Event);
}
