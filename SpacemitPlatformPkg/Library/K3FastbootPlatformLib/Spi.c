/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SpiNorFlash.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "Spi.h"
#include <Library/K3FirmwareUpdateLib.h>

STATIC EFI_SPI_NOR_FLASH_PROTOCOL  *mNor;
EFI_STATUS
K3FastbootSpiInitialize (
  VOID
  )
{
  return K3FirmwareFindFlash (&mNor);
}

VOID
K3FastbootSpiStop (
  VOID
  )
{
  mNor = NULL;
}

BOOLEAN
K3FastbootSpiAvailable (
  VOID
  )
{
  return mNor != NULL;
}

UINT64
K3FastbootSpiSize (
  VOID
  )
{
  return mNor ? mNor->FlashSize : 0;
}

EFI_STATUS
K3FastbootSpiRead (
  UINT64                     Offset,
  UINTN                      Bytes,
  VOID                       *Data,
  FASTBOOT_STORAGE_PROGRESS  Progress OPTIONAL
  )
{
  if (!mNor) {
    return EFI_NOT_READY;
  }

  if (!Data || (Offset >= mNor->FlashSize) || (Bytes > mNor->FlashSize - Offset)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!Bytes) {
    return EFI_SUCCESS;
  }

  EFI_STATUS  Status = Progress ? Progress (0, Bytes, FastbootStorageRead) : EFI_SUCCESS;

  for (UINTN Done = 0; !EFI_ERROR (Status) && Done < Bytes;) {
    UINTN  Part = MIN (SIZE_64KB, Bytes - Done);

    Status = mNor->ReadData (mNor, (UINT32)(Offset + Done), (UINT32)Part, (UINT8 *)Data + Done);
    if (!EFI_ERROR (Status)) {
      Done += Part;
      if (Progress) {
        Status = Progress (Done, Bytes, FastbootStorageRead);
      }
    }
  }

  return Status;
}

STATIC BOOLEAN
Filled (
  CONST UINT8  *Data,
  UINTN        Bytes,
  UINT8        Value
  )
{
  for (UINTN Index = 0; Index < Bytes; Index++) {
    if (Data[Index] != Value) {
      return FALSE;
    }
  }

  return TRUE;
}

EFI_STATUS
K3FastbootSpiFlash (
  CONST VOID                 *Data,
  UINTN                      Bytes,
  FASTBOOT_STORAGE_PROGRESS  Progress
  )
{
  if (!mNor) {
    return EFI_NOT_READY;
  }

  if (!Data || !Bytes || (Bytes > mNor->FlashSize)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  UINT8  *Scratch = AllocatePool (SIZE_4KB);

  if (!Scratch) {
    return EFI_OUT_OF_RESOURCES;
  }

  EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *Fvb = NULL;
  EFI_FVB_ATTRIBUTES_2                Attributes;
  BOOLEAN                             EraseStarted = FALSE;
  CONST UINT8                         *Image       = Data;
  EFI_STATUS                          Status       = EFI_SUCCESS;

  if (Progress) {
    Status = Progress (0, mNor->FlashSize, FastbootStorageErase);
    if (EFI_ERROR (Status)) {
      goto Out;
    }
  }

  Status = K3FirmwareFreezeNv (&Fvb, &Attributes);
  if (EFI_ERROR (Status)) {
    goto Out;
  }

  // The target is the entire device, not a particular firmware format.
  // Erase the whole bank; bytes beyond a short raw payload remain 0xff.
  // SFDP selects the supported erase commands within each submitted range.
  UINT32  EraseBytes = mNor->EraseBlockBytes;

  if ((EraseBytes < SIZE_64KB) && (SIZE_64KB % EraseBytes == 0)) {
    EraseBytes = SIZE_64KB;
  }

  for (UINT32 Offset = 0; Offset < mNor->FlashSize;) {
    UINT32  Part = MIN (EraseBytes, mNor->FlashSize - Offset);
    EraseStarted = TRUE;
    Status       = mNor->Erase (mNor, Offset, Part / SIZE_4KB);
    if (EFI_ERROR (Status)) {
      goto Out;
    }

    Offset += Part;
    if (Progress) {
      Status = Progress (Offset, mNor->FlashSize, FastbootStorageErase);
      if (EFI_ERROR (Status)) {
        goto Out;
      }
    }
  }

  if (Progress) {
    Status = Progress (0, Bytes, FastbootStorageWrite);
    if (EFI_ERROR (Status)) {
      goto Out;
    }
  }

  for (UINTN Offset = 0; Offset < Bytes; ) {
    UINT32  Part = (UINT32)MIN ((UINTN)SIZE_4KB, Bytes - Offset);
    Status = mNor->WriteData (mNor, (UINT32)Offset, Part, (UINT8 *)Image + Offset);
    if (EFI_ERROR (Status)) {
      goto Out;
    }

    Offset += Part;
    if (Progress) {
      Status = Progress (Offset, Bytes, FastbootStorageWrite);
      if (EFI_ERROR (Status)) {
        goto Out;
      }
    }
  }

  if (Progress) {
    Status = Progress (0, mNor->FlashSize, FastbootStorageVerify);
    if (EFI_ERROR (Status)) {
      goto Out;
    }
  }

  for (UINT32 Offset = 0; Offset < mNor->FlashSize; ) {
    UINT32  Part      = MIN (SIZE_4KB, mNor->FlashSize - Offset);
    UINTN   DataBytes = Offset < Bytes ? MIN ((UINTN)Part, Bytes - Offset) : 0;
    Status = mNor->ReadData (mNor, Offset, Part, Scratch);
    if (EFI_ERROR (Status)) {
      goto Out;
    }

    if ((DataBytes && CompareMem (Scratch, Image + Offset, DataBytes)) ||
        !Filled (Scratch + DataBytes, Part - DataBytes, 0xff))
    {
      Status = EFI_CRC_ERROR;
      goto Out;
    }

    Offset += Part;
    if (Progress) {
      Status = Progress (Offset, mNor->FlashSize, FastbootStorageVerify);
      if (EFI_ERROR (Status)) {
        goto Out;
      }
    }
  }

Out:
  // Raw flashing can replace NV. Its old RAM mirror is not writable again
  // until reboot; only an abort before the first erase may undo the freeze.
  if (Fvb && !EraseStarted) {
    EFI_STATUS  Restore = Fvb->SetAttributes (Fvb, &Attributes);
    if (!EFI_ERROR (Status)) {
      Status = Restore;
    }
  }

  FreePool (Scratch);
  return Status;
}

STATIC EFI_STATUS
FirmwareProgress (
  K3_FIRMWARE_PHASE  Phase,
  UINT64             Done,
  UINT64             Total,
  VOID               *Context
  )
{
  STATIC CONST FASTBOOT_STORAGE_PHASE  Phases[] = {
    FastbootStorageRead,  FastbootStorageErase,
    FastbootStorageWrite, FastbootStorageVerify
  };
  FASTBOOT_STORAGE_PROGRESS            Callback = *(FASTBOOT_STORAGE_PROGRESS *)Context;

  return Callback ? Callback (Done, Total, Phases[Phase]) : EFI_SUCCESS;
}

EFI_STATUS
K3FastbootFirmwareFlash (
  CONST VOID                 *Data,
  UINTN                      Bytes,
  FASTBOOT_STORAGE_PROGRESS  Progress
  )
{
  return K3FirmwareUpdate (mNor, Data, Bytes, FirmwareProgress, &Progress);
}
