/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Protocol/UfsDeviceConfig.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UfsStorageLib.h>

EFI_STATUS
UfsFindUserMedia (
  OUT UFS_USER_MEDIA  *Media
  )
{
  EFI_HANDLE      *Handles;
  UINTN           Count;
  EFI_STATUS      Status;
  UFS_USER_MEDIA  Found;

  if (Media == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Media, sizeof (*Media));
  ZeroMem (&Found, sizeof (Found));
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EFI_NOT_FOUND;
  for (UINTN I = 0; I < Count; I++) {
    EFI_BLOCK_IO_PROTOCOL           *Io;
    EFI_DEVICE_PATH_PROTOCOL        *Path, *Node, *Remaining;
    EFI_UFS_DEVICE_CONFIG_PROTOCOL  *Config;
    EFI_HANDLE                      Controller;
    UFS_DEVICE_PATH                 *Ufs = NULL;
    UINT8                           Desc[255];
    UINT32                          Size = sizeof (Desc);
    UINT64                          Blocks;
    EFI_STATUS                      Result;

    Result = gBS->HandleProtocol (Handles[I], &gEfiBlockIoProtocolGuid, (VOID **)&Io);
    if (EFI_ERROR (Result) || !Io || !Io->Media || !Io->Media->MediaPresent ||
        Io->Media->LogicalPartition || !Io->Media->BlockSize)
    {
      continue;
    }

    Path = DevicePathFromHandle (Handles[I]);
    if (!Path || !IsDevicePathValid (Path, 0)) {
      continue;
    }

    for (Node = Path; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
      if ((DevicePathType (Node) == MESSAGING_DEVICE_PATH) && (DevicePathSubType (Node) == MSG_UFS_DP)) {
        if ((DevicePathNodeLength (Node) != sizeof (*Ufs)) || (Ufs != NULL)) {
          Status = EFI_COMPROMISED_DATA;
          goto Done;
        }

        Ufs = (VOID *)Node;
      }
    }

    if (!Ufs || (Ufs->Lun & 0x80)) {
      continue;
    }

    if ((Ufs->Pun != 0) || (Ufs->Lun > 7)) {
      Status = EFI_UNSUPPORTED;
      goto Done;
    }

    Remaining = Path;
    Result    = gBS->LocateDevicePath (&gEfiUfsDeviceConfigProtocolGuid, &Remaining, &Controller);
    if (!EFI_ERROR (Result)) {
      Result = gBS->HandleProtocol (Controller, &gEfiUfsDeviceConfigProtocolGuid, (VOID **)&Config);
    }

    if (EFI_ERROR (Result)) {
      Status = Result;
      goto Done;
    }

    ZeroMem (Desc, sizeof (Desc));
    Result = Config->RwUfsDescriptor (Config, TRUE, 2, Ufs->Lun, 0, Desc, &Size);
    if (EFI_ERROR (Result)) {
      Status = Result;
      goto Done;
    }

    if ((Size < 19) || (Size > sizeof (Desc)) || (Desc[0] < 19) || (Desc[0] > Size) ||
        (Desc[1] != 2) || (Desc[2] != Ufs->Lun))
    {
      Status = EFI_COMPROMISED_DATA;
      goto Done;
    }

    if (!Desc[3] || Desc[4]) {
      continue;
    }

    Blocks = SwapBytes64 (ReadUnaligned64 ((CONST UINT64 *)(Desc + 11)));
    if ((Desc[10] >= 32) || (((UINT64)1 << Desc[10]) != Io->Media->BlockSize) ||
        !Blocks || (Io->Media->LastBlock == MAX_UINT64) ||
        (Blocks != Io->Media->LastBlock + 1) || (Blocks > MAX_UINT64 / Io->Media->BlockSize))
    {
      Status = EFI_COMPROMISED_DATA;
      goto Done;
    }

    if (Found.Io) {
      UINTN  PathSize = GetDevicePathSize (Path);
      if ((PathSize == GetDevicePathSize (Found.Path)) &&
          (CompareMem (Path, Found.Path, PathSize) == 0) && (Found.Bytes == Blocks * Io->Media->BlockSize))
      {
        continue;
      }

      Status = EFI_UNSUPPORTED;
      goto Done;
    }

    Found.Io             = Io;
    Found.Path           = Path;
    Found.Lun            = Ufs->Lun;
    Found.Bytes          = Blocks * Io->Media->BlockSize;
    Found.WriteProtected = Io->Media->ReadOnly || Desc[5] != 0;
  }

  if (Found.Io) {
    *Media = Found;
    Status = EFI_SUCCESS;
  }

Done:
  FreePool (Handles);
  return Status;
}
