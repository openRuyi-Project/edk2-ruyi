/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Uefi/UefiPxe.h>
#include <Guid/EventGroup.h>
#include <Guid/Fdt.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Guid/PremierP550CpuConfig.h>
#include <Library/CpuConfigLib.h>
#include <Library/DebugLib.h>
#include <Library/Eic7700BoardProfileLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/PlatformMacAddressLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#define P550_HIGH_OPP_HZ  1800000000U
#define P550_HIGH_OPP_UV  900000U

STATIC UINT32
ReadCell (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name)
{
  INT32 Length;
  CONST UINT32 *Value = FdtGetProp (Fdt, Node, Name, &Length);

  return (Value != NULL && Length == sizeof (*Value)) ? Fdt32ToCpu (*Value) : 0;
}

STATIC BOOLEAN
P550CpuSupplySupportsHighOpp (CONST VOID *Fdt, INT32 Cpu)
{
  INT32 Supply, Provider, Length;
  UINT32 Min, Max;
  CONST UINT32 *Gpio, *States;

  Supply = FdtNodeOffsetByPhandle (Fdt, ReadCell (Fdt, Cpu, "cpu-supply"));
  if (!FdtNodeEnabled (Fdt, Supply)) { return FALSE; }
  Min = ReadCell (Fdt, Supply, "regulator-min-microvolt");
  Max = ReadCell (Fdt, Supply, "regulator-max-microvolt");
  if (Min == 0 || Min > P550_HIGH_OPP_UV || Max < P550_HIGH_OPP_UV) { return FALSE; }

  if (FdtStringListSearch (Fdt, Supply, "compatible", "regulator-fixed") >= 0) {
    return Min == P550_HIGH_OPP_UV && Max == P550_HIGH_OPP_UV;
  }
  if (FdtStringListSearch (Fdt, Supply, "compatible", "regulator-gpio") < 0) { return FALSE; }

  if (FdtGetProp (Fdt, Supply, "regulator-type", NULL) != NULL &&
      FdtStringListSearch (Fdt, Supply, "regulator-type", "voltage") < 0) {
    return FALSE;
  }

  // The P550 SoM selects 0.9 V on GPIO94 (port C, line 30), active high.
  Gpio = FdtGetProp (Fdt, Supply, "gpios", &Length);
  if (Gpio == NULL || Length != 3 * sizeof (*Gpio) ||
      Fdt32ToCpu (Gpio[1]) != 30 || Fdt32ToCpu (Gpio[2]) != 0) { return FALSE; }
  Provider = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Gpio[0]));
  if (!FdtNodeEnabled (Fdt, Provider) || ReadCell (Fdt, Provider, "reg") != 2 ||
      ReadCell (Fdt, Provider, "#gpio-cells") != 2 ||
      FdtGetProp (Fdt, Provider, "gpio-controller", NULL) == NULL) {
    return FALSE;
  }
  States = FdtGetProp (Fdt, Supply, "states", &Length);
  if (States == NULL || Length <= 0 || Length % (2 * sizeof (*States)) != 0) { return FALSE; }
  for (INT32 I = 0; I < Length / (INT32)sizeof (*States); I += 2) {
    if (Fdt32ToCpu (States[I]) == P550_HIGH_OPP_UV && Fdt32ToCpu (States[I + 1]) == 1) {
      return TRUE;
    }
  }
  return FALSE;
}

STATIC EFI_STATUS
AddP550CpuOpp (VOID *Fdt)
{
  CPU_CONFIG Config;
  EFI_STATUS Status;
  CONST UINT64 *Hz;
  INT32 Cpu, Table, Node, Length;
  UINT32 Voltage = CpuToFdt32 (P550_HIGH_OPP_UV);
  UINT32 Latency = CpuToFdt32 (70000);

  if (FdtStringListSearch (Fdt, 0, "compatible", "sifive,hifive-premier-p550") < 0) {
    return EFI_SUCCESS;
  }
  Status = CpuConfigGet (&Config);
  if (EFI_ERROR (Status) || Config.Default != P550_CPU_ALLOW_1800) { return Status; }

  Cpu = FdtPathOffset (Fdt, "/cpus/cpu@0");
  if (!FdtNodeEnabled (Fdt, Cpu) || !P550CpuSupplySupportsHighOpp (Fdt, Cpu)) {
    return EFI_UNSUPPORTED;
  }
  Table = FdtNodeOffsetByPhandle (Fdt, ReadCell (Fdt, Cpu, "operating-points-v2"));
  if (!FdtNodeEnabled (Fdt, Table) ||
      FdtStringListSearch (Fdt, Table, "compatible", "operating-points-v2") < 0) {
    return EFI_COMPROMISED_DATA;
  }
  for (Node = FdtFirstSubnode (Fdt, Table); Node >= 0; Node = FdtNextSubnode (Fdt, Node)) {
    Hz = FdtGetProp (Fdt, Node, "opp-hz", &Length);
    if ((Hz != NULL) && (Length == sizeof (*Hz)) &&
        (Fdt64ToCpu (ReadUnaligned64 (Hz)) == P550_HIGH_OPP_HZ)) {
      // Preserve any restrictions supplied by the board device tree.
      return EFI_SUCCESS;
    }
  }
  Node = FdtAddSubnode (Fdt, Table, "opp-1800000000");
  if (Node < 0) { return EFI_DEVICE_ERROR; }
  if (FdtSetPropU64 (Fdt, Node, "opp-hz", P550_HIGH_OPP_HZ) ||
      FdtSetProp (Fdt, Node, "opp-microvolt", &Voltage, sizeof (Voltage)) ||
      FdtSetProp (Fdt, Node, "clock-latency-ns", &Latency, sizeof (Latency))) {
    // Discard the private DT copy if rollback itself failed.
    return FdtDelNode (Fdt, Node) == 0 ? EFI_DEVICE_ERROR : EFI_ABORTED;
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
  UINT8 Mac[PXE_HWADDR_LEN_ETHER];
  CONST EIC7700_BOARD_PROFILE *Board = Eic7700GetBoardProfile ();

  gBS->CloseEvent (Event);
  if (Board == NULL) { return; }
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
  for (Index = 0; Index < Board->EthernetPorts; Index++) {
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
  if (Status == EFI_ABORTED) { goto Failed; }
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
