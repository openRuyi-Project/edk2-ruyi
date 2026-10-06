/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/PcdLib.h>

// VariableRuntimeDxe starts after the platform selected emulation or published
// the NOR mirror. Only the flash-backed path needs a mapped store address.
EFI_STATUS EFIAPI
K3NvReadyConstructor (
  EFI_HANDLE        ImageHandle,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return PcdGetBool (PcdEmuVariableNvModeEnable) ||
         PcdGet64 (PcdFlashNvStorageVariableBase64) ? EFI_SUCCESS : EFI_NOT_READY;
}
