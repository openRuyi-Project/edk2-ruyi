/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/Eic7700NvFlashLib.h>
#include "Internal.h"

typedef struct {
  UINT8                    *Readback;
  UINT8                    *TableSector;
  UINTN                    Done;
  UINTN                    Total;
  FIRMWARE_UPDATE_PROGRESS Progress;
  VOID                     *Context;
} D560_UPDATE;

STATIC EFI_STATUS
Notify (D560_UPDATE *Update, FIRMWARE_UPDATE_PHASE Phase)
{
  if (Update->Progress == NULL) {
    return EFI_SUCCESS;
  }
  return Update->Progress (Phase, Update->Done, Update->Total, Update->Context);
}

STATIC EFI_STATUS
ProgramRange (
  D560_UPDATE *Update, CONST D560_FIRMWARE_DIE *Die, EIC7700_SPI_NOR *Remote,
  UINT32 Offset, UINTN Bytes
  )
{
  CONST UINT8 *Source = Die->Image + Offset;
  UINT8 *Before = Update->Readback;
  EFI_STATUS Status = Notify (Update, FirmwareUpdateRead);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Remote ? Eic7700SpiNorReadDevice (Remote, Offset, Bytes, Before)
                  : Eic7700SpiNorRead (Offset, Bytes, Before);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Offset == 0) {
    CopyMem (Update->TableSector, Before, SIZE_4KB);
    CopyMem (Update->TableSector, Source, Die->TableBytes);
    Source = Update->TableSector;
  }

  // Use a block erase only when every sector in the block would need an
  // erase anyway. Keep unchanged sectors and pure 1-to-0 updates intact.
  UINTN Unit = SIZE_4KB;
  if (Bytes == SIZE_64KB) {
    UINTN Pos;
    for (Pos = 0; Pos < Bytes; Pos += SIZE_4KB) {
      UINTN I;
      for (I = Pos; I < Pos + SIZE_4KB; ++I) {
        if ((Before[I] & Source[I]) != Source[I]) {
          break;
        }
      }
      if (I == Pos + SIZE_4KB) {
        break;
      }
    }
    if (Pos == Bytes) {
      Unit = SIZE_64KB;
    }
  }
  for (UINTN Pos = 0; Pos < Bytes; Pos += Unit) {
    if (CompareMem (Before + Pos, Source + Pos, Unit) != 0) {
      Status = Notify (Update, FirmwareUpdateWrite);
      if (!EFI_ERROR (Status)) {
        Status = Eic7700SpiNorReplaceUnit (Remote, Offset + (UINT32)Pos, Unit, Before + Pos, Source + Pos);
      }
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
    Update->Done += Unit;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
CheckVariableStore (UINT8 *Saved, UINT8 *Scratch OPTIONAL)
{
  for (UINT32 Pos = 0; Pos < NV_TOTAL_SIZE; Pos += SIZE_64KB) {
    UINT8 *Buffer = Scratch == NULL ? Saved + Pos : Scratch;
    EFI_STATUS Status = Eic7700SpiNorRead (EIC7700_NV_OFFSET + Pos, SIZE_64KB, Buffer);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (Scratch != NULL && CompareMem (Scratch, Saved + Pos, SIZE_64KB) != 0) {
      return EFI_CRC_ERROR;
    }
  }
  return EFI_SUCCESS;
}

EFI_STATUS
D560ProgramPair (
  CONST D560_FIRMWARE_DIE Dies[D560_DIE_COUNT],
  FIRMWARE_UPDATE_PROGRESS Progress, VOID *Context
  )
{
  D560_UPDATE Update = { .Progress = Progress, .Context = Context };
  D560_REMOTE Remote;
  EFI_STATUS Status, StopStatus;
  EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *Fvb = NULL;
  EFI_FVB_ATTRIBUTES_2 Attributes;
  UINT8 *SavedVariables;

  Update.Total = Dies[0].Size + Dies[1].Size - D560_DIE_COUNT * (D560_STAGE_START - SIZE_4KB);
  Update.Readback = AllocatePool (SIZE_64KB + SIZE_4KB + NV_TOTAL_SIZE);
  if (Update.Readback == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Update.TableSector = Update.Readback + SIZE_64KB;
  SavedVariables = Update.TableSector + SIZE_4KB;
  Status = FirmwareUpdateFreezeNv (&Fvb, &Attributes);
  if (!EFI_ERROR (Status)) {
    Status = CheckVariableStore (SavedVariables, NULL);
  }
  if (!EFI_ERROR (Status)) {
    Status = D560StartRemote (&Remote);
  }
  if (!EFI_ERROR (Status)) {
    // Verify both bodies before publishing either new table. The boot die is last.
    for (UINTN I = D560_DIE_COUNT; I != 0 && !EFI_ERROR (Status); --I) {
      UINTN Die = I - 1;
      for (UINT32 Offset = D560_STAGE_START; Offset < Dies[Die].Size && !EFI_ERROR (Status); ) {
        UINT32 Bytes = MIN (SIZE_64KB, Dies[Die].Size - Offset);
        Status = ProgramRange (&Update, &Dies[Die], Die ? &Remote.Nor : NULL, Offset, Bytes);
        Offset += Bytes;
      }
    }
    for (UINTN I = D560_DIE_COUNT; I != 0 && !EFI_ERROR (Status); --I) {
      UINTN Die = I - 1;
      Status = ProgramRange (&Update, &Dies[Die], Die ? &Remote.Nor : NULL, 0, SIZE_4KB);
    }
    StopStatus = D560StopRemote (&Remote);
    if (!EFI_ERROR (Status)) {
      Status = StopStatus;
    }
  }
  if (!EFI_ERROR (Status)) {
    Status = CheckVariableStore (SavedVariables, Update.Readback);
  }
  if (Fvb != NULL) {
    EFI_STATUS Restore = Fvb->SetAttributes (Fvb, &Attributes);
    if (!EFI_ERROR (Status)) {
      Status = Restore;
    }
  }
  if (!EFI_ERROR (Status)) {
    Status = Notify (&Update, FirmwareUpdateVerify);
  }
  FreePool (Update.Readback);
  return Status;
}
