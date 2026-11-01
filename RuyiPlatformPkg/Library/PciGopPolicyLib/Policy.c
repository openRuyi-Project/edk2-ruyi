/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/PciGopPolicyLib.h>
#include <IndustryStandard/PeImage.h>
#include <Protocol/PciIo.h>
#include <Library/BaseLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC CONST EFI_GUID  mFileGuid = {
  0xd4d2e038, 0xf479, 0x44fa, { 0xa9, 0x78, 0x71, 0x81, 0xb0, 0x8d, 0x53, 0x2d }
};

STATIC CONST UINT8  mImageDigest[SHA256_DIGEST_SIZE] = {
  0x22, 0x02, 0x1e, 0x0a, 0x5a, 0x47, 0xcf, 0xf0, 0xd4, 0x8d, 0xd8, 0x61, 0x61, 0xeb, 0x54, 0x0a,
  0x78, 0x66, 0x04, 0x89, 0x8b, 0x81, 0xda, 0x41, 0xc8, 0xb0, 0x69, 0x83, 0xf7, 0x62, 0x37, 0x2f
};

EFI_STATUS
PciGopReadPciDevice (
  EFI_HANDLE  Controller,
  PCI_TYPE00  *Pci
  )
{
  EFI_PCI_IO_PROTOCOL  *Io;
  EFI_STATUS           Status;

  Status = gBS->HandleProtocol (Controller, &gEfiPciIoProtocolGuid, (VOID **)&Io);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Io->Pci.Read (Io, EfiPciIoWidthUint32, 0, sizeof (*Pci) / sizeof (UINT32), Pci);
}

UINT16
PciGopImageMachine (
  CONST VOID  *Image,
  UINTN       Size
  )
{
  CONST UINT8  *Bytes = Image;
  UINTN        Offset = 0;

  if ((Bytes == NULL) || (Size < sizeof (EFI_IMAGE_DOS_HEADER))) {
    return 0;
  }

  if (ReadUnaligned16 ((CONST UINT16 *)Bytes) == EFI_IMAGE_DOS_SIGNATURE) {
    Offset = ReadUnaligned32 ((CONST UINT32 *)(Bytes + OFFSET_OF (EFI_IMAGE_DOS_HEADER, e_lfanew)));
  }

  if ((Offset > Size) || (Size - Offset < sizeof (UINT32) + sizeof (EFI_IMAGE_FILE_HEADER)) ||
      (ReadUnaligned32 ((CONST UINT32 *)(Bytes + Offset)) != EFI_IMAGE_NT_SIGNATURE))
  {
    return 0;
  }

  return ReadUnaligned16 ((CONST UINT16 *)(Bytes + Offset + sizeof (UINT32)));
}

CONST EFI_GUID *
PciGopFileGuid (
  PCI_GOP_POLICY  Policy
  )
{
  return (Policy == PciGopAmdPreSoc15) ? &mFileGuid : NULL;
}

PCI_GOP_POLICY
PciGopFilePolicy (
  CONST EFI_DEVICE_PATH_PROTOCOL  *Path
  )
{
  if ((Path == NULL) || !IsDevicePathValid (Path, 0)) {
    return PciGopX64;
  }

  while (!IsDevicePathEnd (Path)) {
    if ((DevicePathType (Path) == MEDIA_DEVICE_PATH) &&
        (DevicePathSubType (Path) == MEDIA_PIWG_FW_FILE_DP) &&
        (DevicePathNodeLength (Path) == sizeof (MEDIA_FW_VOL_FILEPATH_DEVICE_PATH)) &&
        IsDevicePathEnd (NextDevicePathNode (Path)))
    {
      if (CompareGuid (&((CONST MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *)Path)->FvFileName, &mFileGuid)) {
        return PciGopAmdPreSoc15;
      }
    }

    Path = NextDevicePathNode (Path);
  }

  return PciGopX64;
}

BOOLEAN
PciGopVerifyImage (
  PCI_GOP_POLICY  Policy,
  CONST VOID      *Image,
  UINTN           Size
  )
{
  UINT8  Digest[SHA256_DIGEST_SIZE];

  return (Policy == PciGopAmdPreSoc15) && (Size == 136896) &&
         (PciGopImageMachine (Image, Size) == EFI_IMAGE_MACHINE_AARCH64) &&
         Sha256HashAll (Image, Size, Digest) &&
         (CompareMem (Digest, mImageDigest, sizeof (Digest)) == 0);
}
