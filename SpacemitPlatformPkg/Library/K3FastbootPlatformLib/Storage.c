/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FastbootPlatformLib.h>
#include <Library/K3EcLib.h>
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


EFI_STATUS
FastbootPlatformInit (
  VOID
  )
{
  EFI_STATUS  Status;
  CHAR8       EcVersion[K3_EC_VERSION_SIZE];

  mTargets = NULL;
  mCount   = 0;
  if (!EFI_ERROR (K3EcGetVersion (EcVersion))) {
    Print (L"EC firmware: %a\r\n", EcVersion);
    Print (L"Fastboot ec: external EC image, explicit activation required\r\n");
  }

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
    if (!EFI_ERROR (FastbootMediaCapacity (mTargets[I].Io, &Bytes))) {
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

STATIC EFI_STATUS
TargetCapacity (CONST CHAR8 *Name, UINT64 *Bytes)
{
  if (!AsciiStrCmp (Name, "ec")) { *Bytes = K3_EC_UPDATE_SIZE; return EFI_SUCCESS; }
  if ((!AsciiStrCmp (Name, K3_SPI_TARGET) || !AsciiStrCmp (Name, K3_FIRMWARE_TARGET)) && K3FastbootSpiAvailable ()) {
    *Bytes = K3FastbootSpiSize (); return EFI_SUCCESS;
  }
  return FastbootMediaCapacity (Target (Name), Bytes);
}

EFI_STATUS
FastbootGetVar (CONST CHAR8 *Name, CHAR8 *Value, UINTN Size)
{
  if (Name == NULL || Value == NULL || Size == 0) { return EFI_INVALID_PARAMETER; }
  if (!AsciiStrCmp (Name, "product")) { return AsciiStrCpyS (Value, Size, (CONST CHAR8 *)PcdGetPtr (PcdSystemProductName)); }
  if (!AsciiStrCmp (Name, "version-ec")) {
    CHAR8 Version[K3_EC_VERSION_SIZE];
    EFI_STATUS Status = K3EcGetVersion (Version);
    return EFI_ERROR (Status) ? Status : AsciiStrCpyS (Value, Size, Version);
  }
  return FastbootGetCommonVariable (Name, Value, Size, TargetCapacity);
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

  if (!AsciiStrCmp (Name, "ec")) {
    if (Offset > MAX_UINT32) {
      return EFI_INVALID_PARAMETER;
    }

    return K3EcFlashRead ((UINT32)Offset, Size, Data, Progress);
  }

  if (!AsciiStrCmp (Name, K3_SPI_TARGET) || !AsciiStrCmp (Name, K3_FIRMWARE_TARGET)) {
    return K3FastbootSpiRead (Offset, Size, Data, Progress);
  }

  return FastbootReadImage (Target (Name), Offset, Size, Data, Progress);
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

  if (!AsciiStrCmp (Name, "ec")) {
    return K3EcUpdate (Data, Size, Progress);
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

CONST CHAR8 *
FastbootFlashInfo (
  CONST CHAR8 *Name
  )
{
  return AsciiStrCmp (Name, "ec") == 0 ?
         "EC verified; use oem reboot-ec to activate" : NULL;
}

EFI_STATUS
FastbootOem (
  CONST CHAR8  *Command,
  CONST CHAR8  **Message
  )
{
  EFI_STATUS Status;

  *Message = NULL;
  if (AsciiStrCmp (Command, "reboot-ec") != 0) {
    return EFI_NOT_FOUND;
  }

  Status = K3EcReboot ();
  if (!EFI_ERROR (Status)) {
    *Message = "Wait 15 seconds, then reboot the board";
  }
  return Status;
}
