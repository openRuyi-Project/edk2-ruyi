/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FirmwareUpdateLib.h>
#include <Library/MemoryAllocationLib.h>
#include "RawSpi.h"

EFI_STATUS
Eic7700FastbootRawFlash (
  EIC7700_SPI_NOR *Nor OPTIONAL, UINT32 Capacity,
  CONST VOID *Data, UINTN Size, FASTBOOT_STORAGE_PROGRESS Progress OPTIONAL
  )
{
  EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *Fvb = NULL;
  EFI_FVB_ATTRIBUTES_2 Attributes;
  EFI_STATUS Status = EFI_SUCCESS;
  BOOLEAN Changed = FALSE;
  UINTN Done = 0;

  if (Data == NULL || Size == 0 || Size > Capacity ||
      Capacity < 2 * SIZE_64KB || Capacity % SIZE_64KB != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  UINT8 *Before = AllocatePool (2 * SIZE_64KB);
  if (Before == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  UINT8 *After = Before + SIZE_64KB;
  // Only the primary chip backs the variable services of this firmware.
  if (Nor == NULL) {
    Status = FirmwareUpdateFreezeNv (&Fvb, &Attributes);
  }
  // Publish sector zero last. Short raw images leave the tail erased.
  for (UINT32 Offset = SIZE_4KB; Done < Capacity && !EFI_ERROR (Status); ) {
    UINT32 Unit = SIZE_4KB;
    if (Offset >= SIZE_64KB && Offset < Capacity - SIZE_64KB && Offset % SIZE_64KB == 0) {
      Unit = SIZE_64KB;
    }
    if (Progress != NULL) {
      Status = Progress (Done, Capacity, FastbootStorageRead);
    }
    if (EFI_ERROR (Status)) { break; }
    Status = Nor == NULL ? Eic7700SpiNorRead (Offset, Unit, Before)
                         : Eic7700SpiNorReadDevice (Nor, Offset, Unit, Before);
    if (EFI_ERROR (Status)) { break; }
    SetMem (After, Unit, 0xff);
    if (Offset < Size) {
      CopyMem (After, (CONST UINT8 *)Data + Offset, MIN (Size - Offset, Unit));
    }
    if (CompareMem (Before, After, Unit) != 0) {
      if (Progress != NULL) {
        Status = Progress (Done, Capacity, FastbootStorageWrite);
      }
      if (EFI_ERROR (Status)) { break; }
      Changed = TRUE;
      Status = Eic7700SpiNorReplaceUnit (Nor, Offset, Unit, Before, After);
      if (EFI_ERROR (Status)) { break; }
    }
    Done += Unit;
    Offset += Unit;
    if (Offset == Capacity) { Offset = 0; }
  }
  if (!EFI_ERROR (Status) && Progress != NULL) {
    Status = Progress (Capacity, Capacity, FastbootStorageVerify);
  }

  // After a raw write the old variable mirror must remain read-only until
  // reboot. Before the first change it is safe to restore its attributes.
  if (!Changed && Fvb != NULL) {
    EFI_STATUS Restore = Fvb->SetAttributes (Fvb, &Attributes);
    if (!EFI_ERROR (Status)) { Status = Restore; }
  }
  FreePool (Before);
  return Status;
}
