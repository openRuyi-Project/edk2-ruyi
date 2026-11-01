/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/NvVarStoreFormatted.h>
#include <Library/FirmwareUpdateLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_STATUS
FirmwareUpdateFreezeNv (
  EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  **Fvb,
  EFI_FVB_ATTRIBUTES_2                *Attributes
  )
{
  EFI_HANDLE  *Handles;
  UINTN       Count;
  EFI_STATUS  Status;

  if (!Fvb || !Attributes) {
    return EFI_INVALID_PARAMETER;
  }

  *Fvb   = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEdkiiNvVarStoreFormattedGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (Status == EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Count != 1) {
    FreePool (Handles);
    return EFI_UNSUPPORTED;
  }

  Status = gBS->HandleProtocol (Handles[0], &gEfiFirmwareVolumeBlockProtocolGuid, (VOID **)Fvb);
  FreePool (Handles);
  // RAM recovery publishes variable readiness without a flash-backed FVB.
  if (Status == EFI_UNSUPPORTED) {
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = (*Fvb)->GetAttributes (*Fvb, Attributes);
  if (!EFI_ERROR (Status)) {
    EFI_FVB_ATTRIBUTES_2  Protected = *Attributes & ~EFI_FVB2_WRITE_STATUS;
    Status = (*Fvb)->SetAttributes (*Fvb, &Protected);
  }

  if (EFI_ERROR (Status)) {
    *Fvb = NULL;
  }

  return Status;
}
