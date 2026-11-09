// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FastbootStorageLib.h>
#include <Library/MemoryAllocationLib.h>

EFI_STATUS
FastbootMediaCapacity (
  EFI_BLOCK_IO_PROTOCOL  *Io,
  UINT64                 *Bytes
  )
{
  if ((Io == NULL) || (Io->Media == NULL) || !Io->Media->MediaPresent) {
    return EFI_NO_MEDIA;
  }

  if (!Io->Media->BlockSize || (Io->Media->LastBlock >= MAX_UINT64 / Io->Media->BlockSize)) {
    return EFI_UNSUPPORTED;
  }

  *Bytes = (Io->Media->LastBlock + 1) * Io->Media->BlockSize;
  return EFI_SUCCESS;
}

EFI_STATUS
FastbootReadImage (
  EFI_BLOCK_IO_PROTOCOL      *Io,
  UINT64                     Offset,
  UINTN                      Size,
  VOID                       *Data,
  FASTBOOT_STORAGE_PROGRESS  Progress OPTIONAL
  )
{
  UINT64                 Bytes;
  UINTN                  Block, Pages = EFI_SIZE_TO_PAGES (SIZE_64KB);
  UINT8                  *Bounce;
  UINT32                 MediaId;
  EFI_STATUS             Status;

  if ((Size != 0) && (Data == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Io == NULL) || (Io->Media == NULL)) {
    return EFI_NO_MEDIA;
  }

  MediaId = Io->Media->MediaId;
  Block   = Io->Media->BlockSize;
  Status  = FastbootMediaCapacity (Io, &Bytes);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Block || SIZE_64KB % Block || (Io->Media->IoAlign > EFI_PAGE_SIZE)) {
    return EFI_UNSUPPORTED;
  }

  if ((Offset > Bytes) || (Size > Bytes - Offset)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if (Size == 0) {
    return EFI_SUCCESS;
  }

  Bounce = AllocatePages (Pages);
  if (Bounce == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  UINTN  Total = Size;

  Status = Progress ? Progress (0, Total, FastbootStorageRead) : EFI_SUCCESS;
  while (!EFI_ERROR (Status) && Size) {
    UINTN  Skip = Offset % Block, Part = MIN (Size, SIZE_64KB - Skip);
    if ((Io->Media == NULL) || !Io->Media->MediaPresent) {
      Status = EFI_NO_MEDIA;
      break;
    }

    if ((Io->Media->MediaId != MediaId) || (Io->Media->BlockSize != Block)) {
      Status = EFI_MEDIA_CHANGED;
      break;
    }

    Status = Io->ReadBlocks (
                   Io,
                   MediaId,
                   Offset / Block,
                   ALIGN_VALUE (Part + Skip, Block),
                   Bounce
                   );
    if (EFI_ERROR (Status)) {
      break;
    }

    CopyMem (Data, Bounce + Skip, Part);
    Data    = (UINT8 *)Data + Part;
    Size   -= Part;
    Offset += Part;
    if (Progress) {
      Status = Progress (Total - Size, Total, FastbootStorageRead);
    }
  }

  FreePages (Bounce, Pages);
  return Status;
}
