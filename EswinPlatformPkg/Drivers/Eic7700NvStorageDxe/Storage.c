/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Protocol/FdtClient.h>
#include <Library/BaseLib.h>
#include <Library/Eic7700SpiNorLib.h>
#include <Library/NvStorageLib.h>
#include <Library/Eic7700NvFlashLib.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_STATUS EFIAPI
Eic7700NvStorageEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  NV_FLASH_OPS Ops;
  EFI_STATUS Status;
  FDT_CLIENT_PROTOCOL *Fdt;
  INT32 Node = -1;
  BOOLEAN D560;

  Status = gBS->LocateProtocol (&gFdtClientProtocolGuid, NULL, (VOID **)&Fdt);
  if (EFI_ERROR (Status)) { return Status; }
  D560 = !EFI_ERROR (Fdt->FindNextCompatibleNode (Fdt, "eswin,eic7702-d560", -1, &Node)) && (Node == 0);

  Node = -1;
  // Current mainline DT has no Boot SPI node. Reserve any vendor node so
  // Linux cannot use the controller concurrently with UEFI variables.
  while (!EFI_ERROR (Fdt->FindNextCompatibleNode (Fdt, "eswin,bootspi", Node, &Node))) {
    if (D560) {
      CONST VOID *Reg;
      UINT32 Length;
      Status = Fdt->GetNodeProperty (Fdt, Node, "reg", &Reg, &Length);
      if (EFI_ERROR (Status) || (Length < sizeof (UINT64))) { return EFI_COMPROMISED_DATA; }
      if (SwapBytes64 (ReadUnaligned64 (Reg)) != 0x51800000) { continue; }
    }
    if (EFI_ERROR (Fdt->SetNodeProperty (Fdt, Node, "status", "reserved", sizeof ("reserved")))) {
      return EFI_DEVICE_ERROR;
    }
  }
  Status = Eic7700NvFlashInitialize (TRUE, &Ops);
  if (EFI_ERROR (Status)) { return Status; }
  // The variable store has a fixed offset on die 0, including in USB recovery.
  return NvStorageInstall (&Ops, EIC7700_NV_OFFSET, Eic7700SpiNorVirtualAddressChange);
}
