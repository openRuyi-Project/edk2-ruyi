/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/Fdt.h>
#include <Library/BaseLib.h>
#include <Library/CpuMmioAddressLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtI2cReadLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include "Pioneer.h"

#define MCU_BOARD_TYPE       0x00
#define MCU_PCB_VERSION      0x02
#define MCU_PIONEER          0x90
#define PCIE_HOST_COMPAT     "sophgo,sg2042-pcie-host"
#define PCIE_SIDEBAND_OFFSET 0x01800000
#define PIPE_MUX             0x000
#define LINK0_CONTROL        0x004
#define LINK0_GEN_SELECT     0x038
#define LINK1_TRAINING       0x160
#define LINK1_CONTROL        0x178
#define SPLIT_PIPE           BIT2
#define ROOT_PORT_MODE       BIT7
#define LINK0_TRAIN_ENABLE   BIT12
#define LINK1_TRAIN_ENABLE   BIT1
#define GEN_SELECT_MASK      7U
#define GEN3_SELECT          2U

STATIC BOOLEAN
IsPioneerV11 (CONST VOID *Fdt)
{
  INT32 Node, Length, Controller;
  CONST UINT32 *Reg;
  UINT8 Type, Version;

  if (FdtStringListSearch (Fdt, 0, "compatible", "milkv,pioneer") < 0) { return FALSE; }
  Node = FdtNodeOffsetByCompatible (Fdt, -1, "sophgo,sg2042-hwmon-mcu");
  if (Node < 0 || !FdtNodeEnabled (Fdt, Node)) { return FALSE; }
  Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  if (!Reg || Length != sizeof (*Reg) || Fdt32ToCpu (*Reg) != 0x17) { return FALSE; }
  Controller = FdtParentOffset (Fdt, Node);
  return !EFI_ERROR (FdtI2cReadRegisters (Fdt, Controller, 0x17, MCU_BOARD_TYPE, &Type, 1)) &&
         Type == MCU_PIONEER &&
         !EFI_ERROR (FdtI2cReadRegisters (Fdt, Controller, 0x17, MCU_PCB_VERSION, &Version, 1)) &&
         Version == 0;
}

INT32
Sg2042PciPortIndex (IN UINTN Base)
{
  STATIC CONST UINTN PortBase[] = { 0x7060000000ULL, 0x7060800000ULL, 0x7062000000ULL, 0x7062800000ULL };

  for (UINTN Index = 0; Index < ARRAY_SIZE (PortBase); Index++) {
    if (PortBase[Index] == Base) { return (INT32)Index; }
  }
  return -1;
}

STATIC BOOLEAN
ReadSplitRootSpeeds (UINTN Base, UINT32 *Speed)
{
  UINT32 Link0, Link1, Gen0, Gen1;

  Base += PCIE_SIDEBAND_OFFSET;
  if (EFI_ERROR (FdtMapMmio (Base, EFI_PAGE_SIZE))) { return FALSE; }
  Base  = CpuMmioAddress (Base);
  Link0 = MmioRead32 (Base + LINK0_CONTROL);
  Gen0  = MmioRead32 (Base + LINK0_GEN_SELECT);
  Link1 = MmioRead32 (Base + LINK1_CONTROL);
  Gen0  = (Gen0 >> 14) & GEN_SELECT_MASK;
  Gen1  = (Link1 >> 8) & GEN_SELECT_MASK;
  Speed[0] = Gen0 + 1;
  Speed[1] = Gen1 + 1;
  return (MmioRead32 (Base + PIPE_MUX) & SPLIT_PIPE) &&
         (Link0 & ROOT_PORT_MODE) && (Link0 & LINK0_TRAIN_ENABLE) &&
         Gen0 <= GEN3_SELECT &&
         (Link1 & ROOT_PORT_MODE) &&
         (MmioRead32 (Base + LINK1_TRAINING) & LINK1_TRAIN_ENABLE) &&
         Gen1 <= GEN3_SELECT;
}

EFI_STATUS
Sg2042PreparePciFdt (OUT VOID **Fdt)
{
  EFI_STATUS Status;
  EFI_PHYSICAL_ADDRESS Address;
  VOID *Original, *Copy;
  UINTN Pages, Base;
  INT32 Node, Index, Port;
  UINT32 Speed[4], Seen = 0;

  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Original);
  if (EFI_ERROR (Status)) { return Status; }
  if (FdtCheckHeader (Original) || FdtTotalSize (Original) > SIZE_1MB) { return EFI_COMPROMISED_DATA; }
  *Fdt = Original;
  if (!IsPioneerV11 (Original)) { return EFI_SUCCESS; }

  for (Node = -1; (Node = FdtNodeOffsetByCompatible (Original, Node, PCIE_HOST_COMPAT)) >= 0;) {
    Index = FdtStringListSearch (Original, Node, "reg-names", "reg");
    if (Index < 0 || EFI_ERROR (FdtGetMmio (Original, Node, Index, EFI_PAGE_SIZE, &Base, NULL))) {
      return EFI_COMPROMISED_DATA;
    }
    Port = Sg2042PciPortIndex (Base);
    if (Port < 0 || (Seen & (1U << Port))) { return EFI_COMPROMISED_DATA; }
    Seen |= 1U << Port;
    if ((Port & 1) == 0 && !ReadSplitRootSpeeds (Base, &Speed[Port])) {
      return EFI_SUCCESS;
    }
  }
  if (Seen != 0xf) { return EFI_SUCCESS; }

  Pages = EFI_SIZE_TO_PAGES (FdtTotalSize (Original) + EFI_PAGE_SIZE);
  Status = gBS->AllocatePages (AllocateAnyPages, EfiACPIReclaimMemory, Pages, &Address);
  if (EFI_ERROR (Status)) { return Status; }
  Copy = (VOID *)(UINTN)Address;
  Status = EFI_COMPROMISED_DATA;
  if (FdtOpenInto (Original, Copy, EFI_PAGES_TO_SIZE (Pages))) { goto Error; }
  for (Node = -1; (Node = FdtNodeOffsetByCompatible (Copy, Node, PCIE_HOST_COMPAT)) >= 0;) {
    UINT32 Limit;
    Index = FdtStringListSearch (Copy, Node, "reg-names", "reg");
    if (Index < 0 || EFI_ERROR (FdtGetMmio (Copy, Node, Index, EFI_PAGE_SIZE, &Base, NULL))) { goto Error; }
    Port = Sg2042PciPortIndex (Base);
    if (Port < 0) { goto Error; }
    Limit = CpuToFdt32 (Speed[Port]);
    if (FdtSetPropString (Copy, Node, "status", "okay") ||
        FdtSetProp (Copy, Node, "max-link-speed", &Limit, sizeof (Limit))) { goto Error; }
  }
  Status = gBS->InstallConfigurationTable (&gFdtTableGuid, Copy);
  if (!EFI_ERROR (Status)) {
    *Fdt = Copy;
    return EFI_SUCCESS;
  }
Error:
  gBS->FreePages (Address, Pages);
  return Status;
}
