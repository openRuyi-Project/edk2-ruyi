/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FastbootPlatformLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include "Spi.h"
#include <Library/UfsStorageLib.h>

STATIC UFS_USER_MEDIA  mUfs;

STATIC FASTBOOT_NVME_TARGET  *mTargets;
STATIC UINTN                 mCount;

STATIC EFI_BLOCK_IO_PROTOCOL *
Target (
  CONST CHAR8  *Name
  )
{
  if (!AsciiStrCmp (Name, "ufs")) {
    return mUfs.Io;
  }

  for (UINTN I = 0; I < mCount; I++) {
    if (!AsciiStrCmp (Name, mTargets[I].Name)) {
      return mTargets[I].Io;
    }
  }

  return NULL;
}

STATIC EFI_STATUS
Capacity (
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
FastbootPlatformInit (
  VOID
  )
{
  EFI_STATUS  Status;

  mTargets = NULL;
  mCount   = 0;
  ZeroMem (&mUfs, sizeof (mUfs));
  Status = UfsFindUserMedia (&mUfs);
  if (!EFI_ERROR (Status)) {
    Print (L"Fastboot ufs: %Lu bytes, user LUN %u\r\n", mUfs.Bytes, mUfs.Lun);
  } else if (Status != EFI_NOT_FOUND) {
    Print (L"Fastboot UFS discovery: %r\r\n", Status);
  }

  Status = FastbootLocateNvme (&mTargets, &mCount);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (UINTN I = 0; I < mCount; I++) {
    UINT64  Bytes;
    if (!EFI_ERROR (Capacity (mTargets[I].Io, &Bytes))) {
      Print (L"Fastboot %a: %Lu MiB\r\n", mTargets[I].Name, Bytes >> 20);
    }
  }

  Status = K3FastbootSpiInitialize ();
  if (!EFI_ERROR (Status)) {
    Print (L"Fastboot %a: %Lu bytes SPI NOR\r\n", K3_SPI_TARGET, K3FastbootSpiSize ());
    Print (L"Fastboot firmware: normal SPI image, preserves UEFI variables\r\n");
  }

  return EFI_SUCCESS;
}

VOID
FastbootPlatformStop (
  VOID
  )
{
  if (mTargets != NULL) {
    FreePool (mTargets);
  }

  mTargets = NULL;
  mCount   = 0;
  K3FastbootSpiStop ();
  ZeroMem (&mUfs, sizeof (mUfs));
}

EFI_STATUS
FastbootGetVar (
  CONST CHAR8  *Name,
  CHAR8        *Value,
  UINTN        Size
  )
{
  CONST CHAR8  *Partition;
  UINT64       Bytes;
  CHAR8        CapacityString[19];

  if ((Name == NULL) || (Value == NULL) || (Size == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!AsciiStrCmp (Name, "product")) {
    return AsciiStrCpyS (Value, Size, (CONST CHAR8 *)PcdGetPtr (PcdSystemProductName));
  }

  if (!AsciiStrCmp (Name, "version-bootloader")) {
    return AsciiStrCpyS (Value, Size, "edk2-ruyi");
  }

  if (!AsciiStrnCmp (Name, "partition-size:", 15) || !AsciiStrnCmp (Name, "partition-type:", 15)) {
    Partition = Name + 15;
  } else if (!AsciiStrnCmp (Name, "has-slot:", 9)) {
    Partition = Name + 9;
  } else {
    return EFI_NOT_FOUND;
  }

  if ((!AsciiStrCmp (Partition, K3_SPI_TARGET) ||
       !AsciiStrCmp (Partition, K3_FIRMWARE_TARGET)) && K3FastbootSpiAvailable ())
  {
    Bytes = K3FastbootSpiSize ();
  } else if (EFI_ERROR (Capacity (Target (Partition), &Bytes))) {
    return EFI_NOT_FOUND;
  }

  if (!AsciiStrnCmp (Name, "partition-size:", 15)) {
    AsciiSPrint (CapacityString, sizeof (CapacityString), "0x%Lx", Bytes);
    return AsciiStrCpyS (Value, Size, CapacityString);
  } else {
    return AsciiStrCpyS (Value, Size, !AsciiStrnCmp (Name, "has-slot:", 9) ? "no" : "raw");
  }

  return EFI_SUCCESS;
}

EFI_STATUS
FastbootRead (
  CONST CHAR8                *Name,
  UINT64                     Offset,
  UINTN                      Size,
  VOID                       *Data,
  FASTBOOT_STORAGE_PROGRESS  Progress OPTIONAL
  )
{
  if ((Name == NULL) || ((Size != 0) && (Data == NULL))) {
    return EFI_INVALID_PARAMETER;
  }

  if (!AsciiStrCmp (Name, K3_SPI_TARGET) || !AsciiStrCmp (Name, K3_FIRMWARE_TARGET)) {
    return K3FastbootSpiRead (Offset, Size, Data, Progress);
  }

  EFI_BLOCK_IO_PROTOCOL  *Io = Target (Name);
  UINT64                 Bytes;
  UINTN                  Block, Pages = EFI_SIZE_TO_PAGES (SIZE_64KB);
  UINT8                  *Bounce;
  UINT32                 MediaId;
  EFI_STATUS             Status;

  if ((Io == NULL) || (Io->Media == NULL)) {
    return EFI_NO_MEDIA;
  }

  MediaId = Io->Media->MediaId;
  Block   = Io->Media->BlockSize;
  Status  = Capacity (Io, &Bytes);

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

EFI_STATUS
FastbootFlash (
  CONST CHAR8                *Name,
  CONST VOID                 *Data,
  UINTN                      Size,
  FASTBOOT_STORAGE_PROGRESS  Progress
  )
{
  if (Name == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!AsciiStrCmp (Name, K3_SPI_TARGET)) {
    return K3FastbootSpiFlash (Data, Size, Progress);
  }

  if (!AsciiStrCmp (Name, K3_FIRMWARE_TARGET)) {
    return K3FastbootFirmwareFlash (Data, Size, Progress);
  }

  EFI_BLOCK_IO_PROTOCOL  *Io = Target (Name);

  if (Io == NULL) {
    return EFI_NOT_FOUND;
  }

  if ((Io == mUfs.Io) && mUfs.WriteProtected) {
    return EFI_WRITE_PROTECTED;
  }

  return FastbootWriteImage (Io, Data, Size, TRUE, Progress);
}
