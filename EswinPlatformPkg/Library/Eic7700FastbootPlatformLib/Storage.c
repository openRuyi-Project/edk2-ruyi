/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <IndustryStandard/Emmc.h>
#include <Protocol/NonDiscoverableDevice.h>
#include <Library/BaseLib.h>
#include <Library/DevicePathLib.h>
#include <Library/Eic7700SpiNorLib.h>
#include <Library/Eic7700BoardProfileLib.h>
#include <Library/D560BootFlashLib.h>
#include "RawSpi.h"
#include <Library/PlatformFirmwareUpdateLib.h>
#include <Library/FastbootPlatformLib.h>
#include <Library/FdtBoardInfoLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#define MMC_HOST_COUNT       3
#define MMC_PARTITION_COUNT  3

STATIC struct {
  CHAR8                    Name[24];
  EFI_BLOCK_IO_PROTOCOL    *Io;
} mCards[MMC_HOST_COUNT * MMC_PARTITION_COUNT];
STATIC FASTBOOT_NVME_TARGET  *mNvme;
STATIC UINTN                 mNvmeCount;
STATIC BOOLEAN               mSpiAvailable;

STATIC BOOLEAN mDualDie;
STATIC UINT32  mSecondaryBytes;

typedef enum {
  NotBootFlash,
  LocalFlash,
  SecondaryFlash,
  FirmwareTarget
} BOOT_FLASH_TARGET;

STATIC BOOT_FLASH_TARGET
BootFlashTarget (CONST CHAR8 *Name)
{
  if (AsciiStrCmp (Name, "firmware") == 0) { return FirmwareTarget; }
  if (AsciiStrCmp (Name, "spi0") == 0 || (!mDualDie && AsciiStrCmp (Name, "spi") == 0)) {
    return LocalFlash;
  }
  if (mDualDie && AsciiStrCmp (Name, "spi1") == 0) { return SecondaryFlash; }
  return NotBootFlash;
}

STATIC EFI_STATUS
SpiProgress (FIRMWARE_UPDATE_PHASE Phase, UINT64 Done, UINT64 Total, VOID *Context)
{
  STATIC CONST FASTBOOT_STORAGE_PHASE Phases[] = {
    FastbootStorageRead, FastbootStorageErase, FastbootStorageWrite, FastbootStorageVerify
  };
  FASTBOOT_STORAGE_PROGRESS Notify = *(FASTBOOT_STORAGE_PROGRESS *)Context;
  return Notify ? Notify (Done, Total, Phases[Phase]) : EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL *
Target (CONST CHAR8 *Name)
{
  UINTN Index;

  for (Index = 0; Index < ARRAY_SIZE (mCards); Index++) {
    if (!AsciiStrCmp (Name, mCards[Index].Name)) {
      return mCards[Index].Io;
    }
  }
  for (Index = 0; Index < mNvmeCount; Index++) {
    if (!AsciiStrCmp (Name, mNvme[Index].Name)) {
      return mNvme[Index].Io;
    }
  }
  return NULL;
}

STATIC INTN
CardPartition (EFI_DEVICE_PATH_PROTOCOL *Path)
{
  UINT32 Partition;

  if (Path == NULL) {
    return -1;
  }
  while (!IsDevicePathEnd (Path)) {
    if ((DevicePathType (Path) == MESSAGING_DEVICE_PATH) &&
        (DevicePathSubType (Path) == MSG_SD_DP) &&
        IsDevicePathEnd (NextDevicePathNode (Path)))
    {
      return EmmcPartitionUserData;
    }
    if ((DevicePathType (Path) == MESSAGING_DEVICE_PATH) &&
        (DevicePathSubType (Path) == MSG_EMMC_DP))
    {
      Path = NextDevicePathNode (Path);
      if ((DevicePathType (Path) != HARDWARE_DEVICE_PATH) ||
          (DevicePathSubType (Path) != HW_CONTROLLER_DP) ||
          (DevicePathNodeLength (Path) != sizeof (CONTROLLER_DEVICE_PATH)) ||
          !IsDevicePathEnd (NextDevicePathNode (Path)))
      {
        return -1;
      }
      Partition = ReadUnaligned32 (&((CONTROLLER_DEVICE_PATH *)Path)->ControllerNumber);
      return Partition <= EmmcPartitionBoot2 ? (INTN)Partition : -1;
    }
    Path = NextDevicePathNode (Path);
  }
  return -1;
}

STATIC INTN
CardHost (CONST VOID *Fdt, EFI_DEVICE_PATH_PROTOCOL *Path)
{
  EFI_DEVICE_PATH_PROTOCOL  *Remaining = Path;
  NON_DISCOVERABLE_DEVICE   *Device;
  EFI_HANDLE                Handle;
  UINTN                     Index, Base;
  CHAR8                     Alias[16];
  CONST CHAR8               *NodePath;
  INT32                     Node;

  if (EFI_ERROR (gBS->LocateDevicePath (&gEdkiiNonDiscoverableDeviceProtocolGuid,
                                        &Remaining, &Handle)) ||
      EFI_ERROR (gBS->HandleProtocol (Handle, &gEdkiiNonDiscoverableDeviceProtocolGuid,
                                      (VOID **)&Device)) || (Device->Resources == NULL))
  {
    return -1;
  }
  for (Index = 0; Index < MMC_HOST_COUNT; Index++) {
    AsciiSPrint (Alias, sizeof (Alias), "mmc%u", (UINT32)Index);
    NodePath = FdtGetAliasNameLen (Fdt, Alias, (INT32)AsciiStrLen (Alias));
    if (NodePath == NULL) {
      continue;
    }
    Node = FdtPathOffset (Fdt, NodePath);
    if (FdtNodeEnabled (Fdt, Node) &&
        !EFI_ERROR (FdtGetMmio (Fdt, Node, 0, 1, &Base, NULL)) &&
        (Device->Resources->AddrRangeMin == Base))
    {
      return (INTN)Index;
    }
  }
  return -1;
}

EFI_STATUS
FastbootPlatformInit (VOID)
{
  EFI_HANDLE             *Handles;
  EFI_BLOCK_IO_PROTOCOL  *Io;
  EFI_STATUS             Status;
  UINTN                  Count, Index;
  INTN                   Card, Host;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  RUYI_BOARD_INFO         Info;
  CONST VOID             *Fdt;
  UINT64                 Bytes;

  Status = FdtBoardInfoGet (&Info, &Fdt);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  mSpiAvailable = !EFI_ERROR (Eic7700SpiNorInit (FALSE));
  CONST EIC7700_BOARD_PROFILE *Board = Eic7700GetBoardProfile ();
  mDualDie = Board != NULL && Board->DualDie;
  mSecondaryBytes = 0;
  if (mDualDie) {
    if (mSpiAvailable) { Print (L"Fastboot spi0: %u bytes (die 0)\r\n", Eic7700SpiNorSize ()); }
    Status = D560SecondaryFlashSize (&mSecondaryBytes);
    if (!EFI_ERROR (Status)) {
      Print (L"Fastboot spi1: %u bytes (die 1)\r\n", mSecondaryBytes);
    } else {
      Print (L"Fastboot spi1 unavailable: %r\r\n", Status);
    }
    if (mSpiAvailable && mSecondaryBytes) {
      Print (L"Fastboot firmware: paired SPI update\r\n");
    }
  } else if (mSpiAvailable) {
    Print (L"Fastboot spi0: %u bytes\r\n", Eic7700SpiNorSize ());
  }
  for (Index = 0; Index < ARRAY_SIZE (mCards); Index++) {
    if (Index % MMC_PARTITION_COUNT == 0) {
      AsciiSPrint (mCards[Index].Name, sizeof (mCards[Index].Name),
                   "mmc%u", (UINT32)(Index / MMC_PARTITION_COUNT));
    } else {
      AsciiSPrint (mCards[Index].Name, sizeof (mCards[Index].Name),
                   "mmc%uboot%u", (UINT32)(Index / MMC_PARTITION_COUNT),
                   (UINT32)(Index % MMC_PARTITION_COUNT - 1));
    }
  }
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid,
                                    NULL, &Count, &Handles);
  if (EFI_ERROR (Status) && Status != EFI_NOT_FOUND) {
    return Status;
  }
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < Count; Index++) {
      Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Io);
      if (EFI_ERROR (Status) || (Io->Media == NULL) || Io->Media->LogicalPartition) {
        continue;
      }
      Path = DevicePathFromHandle (Handles[Index]);
      Card = CardPartition (Path);
      if (Card < 0) {
        continue;
      }
      Host = CardHost (Fdt, Path);
      if (Host < 0) {
        continue;
      }
      Card += Host * MMC_PARTITION_COUNT;
      if (mCards[Card].Io != NULL) {
        FreePool (Handles);
        FastbootPlatformStop ();
        return EFI_COMPROMISED_DATA;
      }
      mCards[Card].Io = Io;
    }
    FreePool (Handles);
  }
  Status = FastbootLocateNvme (&mNvme, &mNvmeCount);
  if (EFI_ERROR (Status)) {
    FastbootPlatformStop ();
    return Status;
  }
  for (Index = 0; Index < ARRAY_SIZE (mCards); Index++) {
    if (!EFI_ERROR (FastbootMediaCapacity (mCards[Index].Io, &Bytes))) {
      Print (L"Fastboot %a: %Lu bytes\r\n", mCards[Index].Name, Bytes);
    }
  }
  for (Index = 0; Index < mNvmeCount; Index++) {
    if (!EFI_ERROR (FastbootMediaCapacity (mNvme[Index].Io, &Bytes))) {
      Print (L"Fastboot %a: %Lu bytes\r\n", mNvme[Index].Name, Bytes);
    }
  }
  return EFI_SUCCESS;
}

VOID
FastbootPlatformStop (VOID)
{
  UINTN Index;

  if (mNvme != NULL) {
    FreePool (mNvme);
  }
  mNvme = NULL;
  mNvmeCount = 0;
  for (Index = 0; Index < ARRAY_SIZE (mCards); Index++) {
    mCards[Index].Io = NULL;
  }
}

STATIC EFI_STATUS
TargetCapacity (CONST CHAR8 *Name, UINT64 *Bytes)
{
  BOOT_FLASH_TARGET Flash = BootFlashTarget (Name);
  if (Flash == SecondaryFlash) {
    if (mSecondaryBytes == 0) { return EFI_NOT_FOUND; }
    *Bytes = mSecondaryBytes;
    return EFI_SUCCESS;
  }
  if (Flash == LocalFlash || Flash == FirmwareTarget) {
    if (!mSpiAvailable) { return EFI_NOT_FOUND; }
    if (mDualDie && Flash == FirmwareTarget) {
      PLATFORM_FIRMWARE_UPDATE_INFO Info;
      if (mSecondaryBytes == 0) { return EFI_NOT_FOUND; }
      PlatformFirmwareUpdateGetInfo (&Info);
      *Bytes = Info.ImageSize;
    } else {
      *Bytes = Eic7700SpiNorSize ();
    }
    return EFI_SUCCESS;
  }
  return FastbootMediaCapacity (Target (Name), Bytes);
}

EFI_STATUS
FastbootGetVar (CONST CHAR8 *Name, CHAR8 *Value, UINTN Size)
{
  if (Name == NULL || Value == NULL || Size == 0) { return EFI_INVALID_PARAMETER; }
  if (!AsciiStrCmp (Name, "product")) {
    RUYI_BOARD_INFO Info;
    EFI_STATUS Status = FdtBoardInfoGet (&Info, NULL);
    return EFI_ERROR (Status) ? Status : AsciiStrCpyS (Value, Size, Info.Model);
  }
  return FastbootGetCommonVariable (Name, Value, Size, TargetCapacity);
}

EFI_STATUS
FastbootFlash (CONST CHAR8 *Name, CONST VOID *Data, UINTN Size,
               FASTBOOT_STORAGE_PROGRESS Progress)
{
  EFI_BLOCK_IO_PROTOCOL *Io;

  if (Name == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  BOOT_FLASH_TARGET Flash = BootFlashTarget (Name);
  if (Flash == SecondaryFlash) {
    D560_REMOTE Remote;
    if (mSecondaryBytes == 0) { return EFI_NOT_FOUND; }
    EFI_STATUS Status = D560StartRemote (&Remote);
    if (EFI_ERROR (Status)) { return Status; }
    Status = Eic7700FastbootRawFlash (&Remote.Nor, Remote.Nor.Size, Data, Size, Progress);
    EFI_STATUS Stop = D560StopRemote (&Remote);
    return EFI_ERROR (Status) ? Status : Stop;
  }
  if (Flash == LocalFlash) {
    if (!mSpiAvailable) { return EFI_NOT_FOUND; }
    return Eic7700FastbootRawFlash (NULL, Eic7700SpiNorSize (), Data, Size, Progress);
  }
  if (Flash == FirmwareTarget) {
    if (!mSpiAvailable || (mDualDie && mSecondaryBytes == 0)) { return EFI_NOT_FOUND; }
    return PlatformFirmwareUpdate (Data, Size, SpiProgress, &Progress);
  }
  Io = Target (Name);
  return Io ? FastbootWriteImage (Io, Data, Size, TRUE, Progress) : EFI_NOT_FOUND;
}

EFI_STATUS
FastbootRead (CONST CHAR8 *Name, UINT64 Offset, UINTN Size, VOID *Data,
              FASTBOOT_STORAGE_PROGRESS Progress)
{
  EFI_BLOCK_IO_PROTOCOL *Io;

  if (Name == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  BOOT_FLASH_TARGET Flash = BootFlashTarget (Name);
  if (Flash == SecondaryFlash) {
    if (mSecondaryBytes == 0) { return EFI_NOT_FOUND; }
    return D560SecondaryFlashRead (Offset, Size, Data, SpiProgress, &Progress);
  }
  if (mDualDie && Flash == FirmwareTarget) { return EFI_UNSUPPORTED; }
  if (Flash == LocalFlash || Flash == FirmwareTarget) {
    UINTN Chunk, Done = 0;
    EFI_STATUS Status;
    if (!mSpiAvailable) { return EFI_NOT_FOUND; }
    if ((Size && !Data) || Offset > Eic7700SpiNorSize () ||
        Size > Eic7700SpiNorSize () - Offset) { return EFI_INVALID_PARAMETER; }
    while (Done < Size) {
      if (Progress) {
        Status = Progress (Done, Size, FastbootStorageRead);
        if (EFI_ERROR (Status)) { return Status; }
      }
      Chunk = MIN (Size - Done, SIZE_64KB);
      Status = Eic7700SpiNorRead ((UINT32)Offset + Done, Chunk, (UINT8 *)Data + Done);
      if (EFI_ERROR (Status)) { return Status; }
      Done += Chunk;
    }
    return Progress ? Progress (Size, Size, FastbootStorageRead) : EFI_SUCCESS;
  }
  Io = Target (Name);
  return Io ? FastbootReadImage (Io, Offset, Size, Data, Progress) : EFI_NOT_FOUND;
}

CONST CHAR8 *
FastbootFlashInfo (CONST CHAR8 *Name)
{
  BOOT_FLASH_TARGET Flash = BootFlashTarget (Name);
  if (Flash == LocalFlash || Flash == SecondaryFlash) {
    return "Raw SPI verified; restart to use it";
  }
  if (Flash == FirmwareTarget) {
    return mDualDie ? "Both SPI flashes verified; restart to use them" : "SPI firmware verified; restart to use it";
  }
  return NULL;
}

EFI_STATUS
FastbootOem (CONST CHAR8 *Command, CONST CHAR8 **Message)
{
  *Message = NULL;
  return EFI_NOT_FOUND;
}
