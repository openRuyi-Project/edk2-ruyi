/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/Fdt.h>
#include <Guid/FdtHob.h>
#include <Guid/RiscVSecHobData.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PlatformMacAddressLib.h>
#include <Library/PrintLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC EFI_STATUS
PublishBoardMacs (
  VOID  *Fdt
  )
{
  INT32  Aliases = FdtPathOffset (Fdt, "/aliases");

  if (Aliases < 0) {
    return EFI_SUCCESS;
  }

  for (UINTN Index = 0; Index < 16; Index++) {
    CHAR8        Name[24];
    INT32        Length, Node;
    CONST CHAR8  *Path;
    UINT8        Mac[6];

    /* Property insertion can move the aliases node in the FDT blob. */
    Aliases = FdtPathOffset (Fdt, "/aliases");
    AsciiSPrint (Name, sizeof (Name), "ethernet%u", Index);
    Path = FdtGetProp (Fdt, Aliases, Name, &Length);
    if (!Path || (Length <= 1) || (Path[Length - 1] != '\0')) {
      continue;
    }

    Node = FdtPathOffset (Fdt, Path);
    if ((Node < 0) ||
        ((FdtStringListSearch (
            Fdt,
            Node,
            "compatible",
            "spacemit,k3-dwmac"
            ) < 0) &&
         (FdtStringListSearch (
            Fdt,
            Node,
            "compatible",
            "pci10ec,8127"
            ) < 0)))
    {
      continue;
    }

    EFI_STATUS  Status = PlatformGetMacAddress (Index, Mac);

    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_WARN,
        "FDT Ethernet %u MAC unavailable: %r\n",
        Index,
        Status
        ));
      continue;
    }

    if (FdtSetProp (Fdt, Node, "mac-address", Mac, sizeof (Mac)) < 0) {
      return EFI_OUT_OF_RESOURCES;
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
PublishFdt (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  CONST EFI_GUID          HandoffGuid = RISCV_SEC_HANDOFF_HOB_GUID;
  EFI_HOB_GUID_TYPE       *Hob;
  RISCV_SEC_HANDOFF_DATA  Handoff;
  VOID                    *Fdt, *Copy;
  UINTN                   Size;
  UINT32                  Hart;
  INT32                   Chosen;
  EFI_STATUS              Status;

  Hob = GetFirstGuidHob (&HandoffGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (Handoff))) {
    return EFI_NOT_FOUND;
  }

  CopyMem (&Handoff, GET_GUID_HOB_DATA (Hob), sizeof (Handoff));
  Fdt = Handoff.FdtPointer;
  if ((Fdt == NULL) || (FdtCheckHeader (Fdt) != 0) ||
      (Handoff.BootHartId > MAX_UINT32))
  {
    return EFI_COMPROMISED_DATA;
  }

  Size = FdtTotalSize (Fdt);
  if ((Size < sizeof (FDT_HEADER)) || (Size > SIZE_1MB)) {
    return EFI_COMPROMISED_DATA;
  }

  Copy = AllocateRuntimeZeroPool (Size + SIZE_4KB);
  if (Copy == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (FdtOpenInto (Fdt, Copy, Size + SIZE_4KB) != 0) {
    FreePool (Copy);
    return EFI_COMPROMISED_DATA;
  }

  Chosen = FdtPathOffset (Copy, "/chosen");
  if (Chosen < 0) {
    Chosen = FdtAddSubnode (Copy, 0, "chosen");
  }

  Hart = CpuToFdt32 ((UINT32)Handoff.BootHartId);
  if ((Chosen < 0) || (FdtSetProp (Copy, Chosen, "boot-hartid", &Hart, sizeof (Hart)) != 0)) {
    FreePool (Copy);
    return EFI_OUT_OF_RESOURCES;
  }

  // The real variable store owns QSPI through UEFI runtime services.
  // Firmware drivers keep using the original HOB, not this OS handoff copy.
  if (!PcdGetBool (PcdEmuVariableNvModeEnable)) {
    INT32  Qspi = FdtNodeOffsetByCompatible (Copy, -1, "spacemit,k3-qspi");
    if ((Qspi < 0) || (FdtSetProp (Copy, Qspi, "status", "disabled", sizeof ("disabled")) < 0)) {
      FreePool (Copy);
      return EFI_DEVICE_ERROR;
    }
  }

  Status = PublishBoardMacs (Copy);
  if (EFI_ERROR (Status)) {
    FreePool (Copy);
    return Status;
  }

  Status = gBS->InstallConfigurationTable (&gFdtTableGuid, Copy);
  if (EFI_ERROR (Status)) {
    FreePool (Copy);
  }

  return Status;
}
