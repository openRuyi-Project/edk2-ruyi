/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/NvVarStoreFormatted.h>
#include <Guid/EventGroup.h>
#include <Protocol/DevicePath.h>
#include <Protocol/FaultTolerantWrite.h>
#include <Protocol/SpiNorFlash.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/K3NvFvbLib.h>
#include <Library/HobLib.h>
#include <Pi/PiBootMode.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include "Flash.h"

STATIC K3_NV_FVB                   mFvb;
STATIC EFI_SPI_NOR_FLASH_PROTOCOL  *mNor;
STATIC EFI_HANDLE                  mHandle;
STATIC EFI_EVENT                   mFtwEvent;
STATIC VOID                        *mFtwRegistration;
STATIC struct {
  MEMMAP_DEVICE_PATH          Memory;
  EFI_DEVICE_PATH_PROTOCOL    End;
} mPath = {
  {
    { HARDWARE_DEVICE_PATH, HW_MEMMAP_DP,                   { sizeof (MEMMAP_DEVICE_PATH),       0 }
    },
    EfiRuntimeServicesData, 0, 0
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC VOID EFIAPI
VirtualAddressChange (
  EFI_EVENT  Event,
  VOID       *Context
  )
{
  K3NvFlashVirtualAddressChange ();
  EfiConvertPointer (0, (VOID **)&mFvb.Mirror);
  mFvb.Physical = (UINTN)mFvb.Mirror;
  EfiConvertPointer (0, (VOID **)&mFvb.Flash.Read);
  EfiConvertPointer (0, (VOID **)&mFvb.Flash.Program);
  EfiConvertPointer (0, (VOID **)&mFvb.Flash.Erase);
  EfiConvertPointer (0, (VOID **)&mFvb.Protocol.GetAttributes);
  EfiConvertPointer (0, (VOID **)&mFvb.Protocol.SetAttributes);
  EfiConvertPointer (0, (VOID **)&mFvb.Protocol.GetPhysicalAddress);
  EfiConvertPointer (0, (VOID **)&mFvb.Protocol.GetBlockSize);
  EfiConvertPointer (0, (VOID **)&mFvb.Protocol.Read);
  EfiConvertPointer (0, (VOID **)&mFvb.Protocol.Write);
  EfiConvertPointer (0, (VOID **)&mFvb.Protocol.EraseBlocks);
}

STATIC EFI_STATUS
FindFlash (
  VOID
  )
{
  EFI_HANDLE  *Handles;
  UINTN       Count;
  EFI_STATUS  Status = gBS->LocateHandleBuffer (
                              ByProtocol,
                              &gEfiSpiNorFlashProtocolGuid,
                              NULL,
                              &Count,
                              &Handles
                              );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (UINTN I = 0; I < Count; I++) {
    EFI_SPI_NOR_FLASH_PROTOCOL  *Nor;
    if (EFI_ERROR (
          gBS->HandleProtocol (
                 Handles[I],
                 &gEfiSpiNorFlashProtocolGuid,
                 (VOID **)&Nor
                 )
          ) ||
        !Nor->SpiPeripheral || !Nor->SpiPeripheral->SpiBus)
    {
      continue;
    }

    CONST MEMMAP_DEVICE_PATH  *Path = (CONST VOID *)Nor->SpiPeripheral->SpiBus->ControllerPath;
    if (!Path || (Path->Header.Type != HARDWARE_DEVICE_PATH) ||
        (Path->Header.SubType != HW_MEMMAP_DP) ||
        ((Path->Header.Length[0] | (Path->Header.Length[1] << 8)) != sizeof (*Path)) ||
        (Path->StartingAddress != 0xd420c000) || (Path->EndingAddress != 0xd420cfff))
    {
      continue;
    }

    if (mNor || (Nor->FlashSize < K3_NV_FLASH_OFFSET + K3_NV_TOTAL_SIZE) ||
        (Nor->EraseBlockBytes != K3_NV_BLOCK_SIZE) ||
        !Nor->ReadData || !Nor->WriteData || !Nor->Erase)
    {
      Status = EFI_UNSUPPORTED;
      goto Done;
    }

    mNor = Nor;
  }

  Status = mNor ? EFI_SUCCESS : EFI_NOT_FOUND;
Done:
  FreePool (Handles);
  return Status;
}

STATIC VOID EFIAPI
FtwReady (
  EFI_EVENT  Event,
  VOID       *Context
  )
{
  EFI_FAULT_TOLERANT_WRITE_PROTOCOL  *Ftw;

  if (!mHandle || EFI_ERROR (
                    gBS->LocateProtocol (
                           &gEfiFaultTolerantWriteProtocolGuid,
                           NULL,
                           (VOID **)&Ftw
                           )
                    ))
  {
    return;
  }

  // FTW may have replayed an interrupted reclaim. Reload and validate that
  // result before allowing VariableRuntimeDxe to cache the variable store.
  K3_NV_FLASH_OPS  Ops     = mFvb.Flash;
  VOID             *Mirror = mFvb.Mirror;
  EFI_STATUS       Status  = K3NvFvbInitialize (&mFvb, Mirror, &Ops);

  if (!EFI_ERROR (Status)) {
    Status = gBS->InstallProtocolInterface (
                    &mHandle,
                    &gEdkiiNvVarStoreFormattedGuid,
                    EFI_NATIVE_INTERFACE,
                    NULL
                    );
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "K3 NV store is not ready after FTW recovery: %r\n", Status));
  }

  gBS->CloseEvent (Event);
  mFtwEvent = NULL;
}

EFI_STATUS EFIAPI
K3NvStorageEntry (
  EFI_HANDLE        ImageHandle,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS       Status;
  EFI_EVENT        VirtualEvent = NULL;
  UINTN            Pages        = EFI_SIZE_TO_PAGES (K3_NV_TOTAL_SIZE);
  K3_NV_FLASH_OPS  Ops;

  BOOLEAN  Recovery = GetBootModeHob () == BOOT_IN_RECOVERY_MODE;

  Status = PcdSetBoolS (PcdEmuVariableNvModeEnable, Recovery);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Recovery) {
    // Unblock VariableRuntimeDxe in emulation mode without exposing an FVB.
    return gBS->InstallProtocolInterface (
                  &mHandle,
                  &gEdkiiNvVarStoreFormattedGuid,
                  EFI_NATIVE_INTERFACE,
                  NULL
                  );
  }

  Status = FindFlash ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3NvFlashInitialize (mNor->FlashSize, &Ops);
  mNor   = NULL;
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "K3 runtime NV flash initialization failed: %r\n", Status));
    return Status;
  }

  VOID  *Mirror = AllocateAlignedRuntimePages (Pages, K3_NV_WORKING_SIZE);

  if (!Mirror) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = K3NvFvbInitialize (&mFvb, Mirror, &Ops);
  // An interrupted reclaim can leave the header invalid. Publish the block
  // interface for FTW to repair it, without yet publishing variable readiness.
  if (EFI_ERROR (Status) && (Status != EFI_VOLUME_CORRUPTED)) {
    DEBUG ((DEBUG_ERROR, "K3 NV storage initialization failed: %r\n", Status));
    goto Error;
  }

  Status = PcdSet64S (PcdFlashNvStorageVariableBase64, (UINTN)Mirror);
  if (!EFI_ERROR (Status)) {
    Status = PcdSet64S (PcdFlashNvStorageFtwWorkingBase64, (UINTN)Mirror + K3_NV_VARIABLE_SIZE);
  }

  if (!EFI_ERROR (Status)) {
    Status = PcdSet64S (
               PcdFlashNvStorageFtwSpareBase64,
               (UINTN)Mirror + K3_NV_VARIABLE_SIZE + K3_NV_WORKING_SIZE
               );
  }

  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  VirtualAddressChange,
                  NULL,
                  &gEfiEventVirtualAddressChangeGuid,
                  &VirtualEvent
                  );
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  mPath.Memory.StartingAddress = (UINTN)Mirror;
  mPath.Memory.EndingAddress   = (UINTN)Mirror + K3_NV_TOTAL_SIZE - 1;
  mFtwEvent                    = EfiCreateProtocolNotifyEvent (
                                   &gEfiFaultTolerantWriteProtocolGuid,
                                   TPL_CALLBACK,
                                   FtwReady,
                                   NULL,
                                   &mFtwRegistration
                                   );
  if (!mFtwEvent) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Error;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mPath,
                  &gEfiFirmwareVolumeBlockProtocolGuid,
                  &mFvb.Protocol,
                  &gSpacemitNvFvbReadyGuid,
                  NULL,
                  NULL
                  );
  if (!EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

Error:
  if (mFtwEvent) {
    gBS->CloseEvent (mFtwEvent);
    mFtwEvent = NULL;
  }

  if (VirtualEvent) {
    gBS->CloseEvent (VirtualEvent);
  }

  FreeAlignedPages (Mirror, Pages);
  return Status;
}
