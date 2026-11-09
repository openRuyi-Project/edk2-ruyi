/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Protocol/FdtClient.h>
#include <Library/Eic7700SpiNorLib.h>
#include <Library/NvStorageLib.h>
#include <Library/PremierP550NvFlashLib.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_STATUS EFIAPI
PremierP550NvStorageEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  NV_FLASH_OPS Ops;
  EFI_STATUS Status;
  FDT_CLIENT_PROTOCOL *Fdt;
  INT32 Node = -1;

  Status = gBS->LocateProtocol (&gFdtClientProtocolGuid, NULL, (VOID **)&Fdt);
  if (EFI_ERROR (Status)) { return Status; }
  // Current mainline DT has no Boot SPI node. Reserve any vendor node so
  // Linux cannot use the controller concurrently with UEFI variables.
  while (!EFI_ERROR (Fdt->FindNextCompatibleNode (Fdt, "eswin,bootspi", Node, &Node))) {
    if (EFI_ERROR (Fdt->SetNodeProperty (Fdt, Node, "status", "reserved", sizeof ("reserved")))) {
      return EFI_DEVICE_ERROR;
    }
  }
  Status = PremierP550NvFlashInitialize (TRUE, &Ops);
  if (EFI_ERROR (Status)) { return Status; }
  return NvStorageInstall (&Ops, P550_NV_OFFSET, Eic7700SpiNorVirtualAddressChange);
}
