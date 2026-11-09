/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Guid/NvVarStoreFormatted.h>
#include <Protocol/DevicePath.h>
#include <Protocol/FaultTolerantWrite.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NvStorageLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeLib.h>

STATIC NV_FLASH_VIRTUAL_CHANGE mConvertFlash;
STATIC NV_FVB      mFvb;
STATIC EFI_HANDLE  mHandle;
STATIC EFI_EVENT   mFtwEvent;
STATIC VOID        *mFtwRegistration;
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
  mConvertFlash ();
  EfiConvertPointer (EFI_OPTIONAL_PTR, &mFvb.Flash.Context);
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
  NV_FLASH_OPS  Ops     = mFvb.Flash;
  VOID          *Mirror = mFvb.Mirror;
  EFI_STATUS    Status  = NvFvbInitialize (&mFvb, Mirror, &Ops, mFvb.FlashOffset);

  if (!EFI_ERROR (Status)) {
    Status = gBS->InstallProtocolInterface (
                    &mHandle,
                    &gEdkiiNvVarStoreFormattedGuid,
                    EFI_NATIVE_INTERFACE,
                    NULL
                    );
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "NV store is not ready after FTW recovery: %r\n", Status));
  }

  gBS->CloseEvent (Event);
  mFtwEvent = NULL;
}

EFI_STATUS
NvStorageInstall (
  CONST NV_FLASH_OPS       *Flash,
  UINT32                   FlashOffset,
  NV_FLASH_VIRTUAL_CHANGE   ConvertFlash
  )
{
  EFI_STATUS Status;
  EFI_EVENT VirtualEvent = NULL;
  UINTN Pages = EFI_SIZE_TO_PAGES (NV_TOTAL_SIZE);

  if (!Flash || !Flash->Read || !Flash->Program || !Flash->Erase || !ConvertFlash) {
    return EFI_INVALID_PARAMETER;
  }
  if (mHandle) { return EFI_ALREADY_STARTED; }
  mConvertFlash = ConvertFlash;

  VOID  *Mirror = AllocateAlignedRuntimePages (Pages, NV_WORKING_SIZE);

  if (!Mirror) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = NvFvbInitialize (&mFvb, Mirror, Flash, FlashOffset);
  // An interrupted reclaim can leave the header invalid. Publish the block
  // interface for FTW to repair it, without yet publishing variable readiness.
  if (EFI_ERROR (Status) && (Status != EFI_VOLUME_CORRUPTED)) {
    DEBUG ((DEBUG_ERROR, "NV storage initialization failed: %r\n", Status));
    goto Error;
  }

  Status = PcdSet64S (PcdFlashNvStorageVariableBase64, (UINTN)Mirror);
  if (!EFI_ERROR (Status)) {
    Status = PcdSet64S (PcdFlashNvStorageFtwWorkingBase64, (UINTN)Mirror + NV_VARIABLE_SIZE);
  }

  if (!EFI_ERROR (Status)) {
    Status = PcdSet64S (
               PcdFlashNvStorageFtwSpareBase64,
               (UINTN)Mirror + NV_VARIABLE_SIZE + NV_WORKING_SIZE
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
  mPath.Memory.EndingAddress   = (UINTN)Mirror + NV_TOTAL_SIZE - 1;
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
                  &gRuyiNvFvbReadyGuid,
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
