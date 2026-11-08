/** @file
  K3 backends for the EmbeddedPkg Fastboot application.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <PiDxe.h>
#include <Pi/PiBootMode.h>
#include <Protocol/AndroidFastbootPlatform.h>
#include <Protocol/AndroidFastbootPlatformExtension.h>
#include <Protocol/GblEfiFastbootTransport.h>
#include <Library/BaseLib.h>
#include <Library/FastbootPlatformLib.h>
#include <Library/FastbootTransportLib.h>
#include <Library/HobLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#define TRANSFER_BYTES  SIZE_16KB

STATIC BOOLEAN                 mRunning;
STATIC BOOLEAN                 mRestart;
STATIC BOOLEAN                 mSessionInvalid;
STATIC CONST CHAR8             *mTarget;
STATIC UINT64                  mLastProgress;
STATIC BOOLEAN                 mFirstProgress;
STATIC FASTBOOT_STORAGE_PHASE  mProgressPhase;
STATIC EFI_STATUS              mProgressStatus;

STATIC EFI_STATUS EFIAPI
TransportStart (
  IN GBL_EFI_FASTBOOT_TRANSPORT_PROTOCOL  *This
  )
{
  EFI_STATUS  Status;

  Status          = FastbootUsbStart ();
  mRunning        = !EFI_ERROR (Status);
  mRestart        = FALSE;
  mSessionInvalid = FALSE;
  return Status;
}

STATIC EFI_STATUS EFIAPI
TransportStop (
  IN GBL_EFI_FASTBOOT_TRANSPORT_PROTOCOL  *This
  )
{
  EFI_STATUS  Status;

  Status = FastbootUsbStop ();
  if (!EFI_ERROR (Status)) {
    mRunning        = FALSE;
    mRestart        = FALSE;
    mSessionInvalid = FALSE;
  }

  return Status;
}

STATIC EFI_STATUS
TransportStatus (
  IN EFI_STATUS  Status
  )
{
  if (Status == EFI_MEDIA_CHANGED) {
    // Suppress responses from an operation interrupted by this reset.
    // Receive acknowledges the new session without restarting the controller.
    mSessionInvalid = TRUE;
    return EFI_DEVICE_ERROR;
  }

  if (EFI_ERROR (Status) && (Status != EFI_NOT_READY) &&
      (Status != EFI_INVALID_PARAMETER) && (Status != EFI_BUFFER_TOO_SMALL))
  {
    mRestart = TRUE;
    return EFI_DEVICE_ERROR;
  }

  return Status;
}

STATIC EFI_STATUS EFIAPI
TransportReceive (
  IN GBL_EFI_FASTBOOT_TRANSPORT_PROTOCOL  *This,
  IN OUT UINTN                            *BufferSize,
  OUT VOID                                *Buffer,
  IN UINT32                               Mode
  )
{
  EFI_STATUS  Status;
  UINTN       Bytes;

  if ((BufferSize == NULL) || (Buffer == NULL) || (*BufferSize == 0) ||
      ((Mode != GblEfiFastbootRxModeSinglePacket) && (Mode != GblEfiFastbootRxModeFixedLength)))
  {
    return EFI_INVALID_PARAMETER;
  }

  mSessionInvalid = FALSE;
  if (mRestart) {
    Status = TransportStop (This);
    if (!EFI_ERROR (Status)) {
      Status = TransportStart (This);
    }

    if (EFI_ERROR (Status)) {
      // The application must stop and retain DMA if recovery cannot halt it.
      return EFI_ABORTED;
    }
  }

  if (!mRunning) {
    return EFI_NOT_STARTED;
  }

  // Process connection events before selecting the negotiated packet size.
  Status = TransportStatus (FastbootUsbPoll ());
  if (EFI_ERROR (Status)) {
    *BufferSize = 0;
    return Status;
  }

  // Command receives use one packet. Data receives preserve the remaining
  // length, including a packet-aligned tail that does not carry a host ZLP.
  Bytes       = MIN (*BufferSize, Mode == GblEfiFastbootRxModeSinglePacket ? FastbootUsbPacketSize () : TRANSFER_BYTES);
  Status      = FastbootUsbReceive (Buffer, &Bytes);
  *BufferSize = !EFI_ERROR (Status) || Status == EFI_BUFFER_TOO_SMALL ? Bytes : 0;
  return TransportStatus (Status);
}

STATIC EFI_STATUS EFIAPI
TransportSend (
  IN GBL_EFI_FASTBOOT_TRANSPORT_PROTOCOL  *This,
  IN OUT UINTN                            *BufferSize,
  IN CONST VOID                           *Buffer
  )
{
  EFI_STATUS  Status;
  UINTN       Bytes;

  if ((BufferSize == NULL) || ((Buffer == NULL) && (*BufferSize != 0))) {
    return EFI_INVALID_PARAMETER;
  }

  if (!mRunning) {
    return EFI_NOT_STARTED;
  }

  if (mRestart || mSessionInvalid) {
    *BufferSize = 0;
    return EFI_DEVICE_ERROR;
  }

  Bytes       = MIN (*BufferSize, TRANSFER_BYTES);
  Status      = FastbootUsbSend (Buffer, Bytes);
  *BufferSize = EFI_ERROR (Status) ? 0 : Bytes;
  return TransportStatus (Status);
}

STATIC EFI_STATUS EFIAPI
TransportFlush (
  IN GBL_EFI_FASTBOOT_TRANSPORT_PROTOCOL  *This
  )
{
  // FastbootUsbSend waits for completion before returning.
  if (!mRunning) {
    return EFI_NOT_STARTED;
  }

  if (mRestart || mSessionInvalid) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC GBL_EFI_FASTBOOT_TRANSPORT_PROTOCOL  mTransport = {
  GBL_EFI_FASTBOOT_TRANSPORT_PROTOCOL_REVISION,
  "K3 USB",
  TransportStart,
  TransportStop,
  TransportReceive,
  TransportSend,
  TransportFlush
};

STATIC EFI_STATUS
SendInfo (
  IN CONST CHAR8  *Text
  )
{
  CHAR8       Reply[65];
  UINTN       Bytes;
  EFI_STATUS  Status;

  AsciiSPrint (Reply, sizeof (Reply), "INFO%a", Text);
  Bytes  = AsciiStrLen (Reply);
  Status = TransportSend (&mTransport, &Bytes, Reply);
  return EFI_ERROR (Status) ? Status : TransportFlush (&mTransport);
}

STATIC EFI_STATUS
FlashProgress (
  UINT64                  Done,
  UINT64                  Total,
  FASTBOOT_STORAGE_PHASE  Phase
  )
{
  STATIC CONST CHAR8  *Labels[] = { "erase", "write", "verify", "read" };
  CHAR8               Info[61];
  CHAR8               Line[128];
  EFI_INPUT_KEY       Key;
  UINT64              Now;
  UINTN               Shift;
  UINT64              DoneUnits;
  UINT64              TotalUnits;
  UINT32              Percent;
  CONST CHAR8         *Unit;

  if ((UINTN)Phase >= ARRAY_SIZE (Labels)) {
    return EFI_INVALID_PARAMETER;
  }

  mProgressStatus = TransportStatus (FastbootUsbPoll ());
  if (EFI_ERROR (mProgressStatus)) {
    return mProgressStatus;
  }

  if (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key)) && (Key.ScanCode == SCAN_ESC)) {
    return EFI_ABORTED;
  }

  Now = GetPerformanceCounter ();
  if (mFirstProgress || (mProgressPhase != Phase) || (Done == Total) ||
      (GetTimeInNanoSecond (Now - mLastProgress) >= 1000000000ULL))
  {
    Percent    = Done == Total ? 100 : (UINT32)MIN (99, (Done >> 10) * 100 / MAX (Total >> 10, 1));
    Shift      = Total != 0 && Total < SIZE_1MB ? 10 : 20;
    Unit       = Shift == 10 ? "KiB" : "MiB";
    TotalUnits = (Total >> Shift) + ((Total & ((1ULL << Shift) - 1)) != 0);
    DoneUnits  = Done == Total ? TotalUnits : Done >> Shift;
    AsciiSPrint (
      Line,
      sizeof (Line),
      "\rFastboot %a %a: %Lu/%Lu %a (%u%%)    %a",
      mTarget,
      Labels[Phase],
      DoneUnits,
      TotalUnits,
      Unit,
      Percent,
      Done == Total ? "\r\n" : ""
      );
    SerialPortWrite ((UINT8 *)Line, AsciiStrLen (Line));
    AsciiSPrint (Info, sizeof (Info), "%a %Lu/%Lu %a", Labels[Phase], DoneUnits, TotalUnits, Unit);
    mProgressStatus = SendInfo (Info);
    mFirstProgress  = FALSE;
    mProgressPhase  = Phase;
    mLastProgress   = GetPerformanceCounter ();
  }

  return mProgressStatus;
}

STATIC VOID
BeginOperation (
  IN CONST CHAR8  *Name
  )
{
  mTarget         = Name;
  mFirstProgress  = TRUE;
  mProgressStatus = EFI_SUCCESS;
}

STATIC EFI_STATUS
PlatformInit (
  VOID
  )
{
  SerialPortInitialize ();
  Print (
    GetBootModeHob () == BOOT_IN_RECOVERY_MODE ?
    L"RAM Recovery - USB Fastboot\r\n" : L"USB Fastboot\r\n"
    );
  return FastbootPlatformInit ();
}

STATIC EFI_STATUS
PlatformGetVar (
  IN CHAR8   *Name,
  OUT CHAR8  *Value
  )
{
  return FastbootGetVar (Name, Value, 61);
}

STATIC EFI_STATUS
PlatformFlash (
  IN CHAR8  *Name,
  IN UINTN  Size,
  IN VOID   *Buffer
  )
{
  EFI_STATUS  Status;

  BeginOperation (Name);
  Status = FastbootFlash (Name, Buffer, Size, FlashProgress);
  if (EFI_ERROR (mProgressStatus)) {
    return mProgressStatus;
  }

  if (!EFI_ERROR (Status) && (AsciiStrCmp (Name, "ec") == 0)) {
    return SendInfo ("EC verified; use oem reboot-ec to activate");
  }

  return Status;
}

STATIC EFI_STATUS
PlatformErase (
  IN CHAR8  *Name
  )
{
  return EFI_UNSUPPORTED;
}

STATIC EFI_STATUS
PlatformOem (
  IN CHAR8  *Command
  )
{
  EFI_STATUS  Status;

  if (AsciiStrCmp (Command, "reboot-ec") != 0) {
    return EFI_NOT_FOUND;
  }

  Status = FastbootRebootEc ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return SendInfo ("Wait 15 seconds, then reboot the board");
}

STATIC EFI_STATUS EFIAPI
PlatformRead (
  IN FASTBOOT_PLATFORM_EXTENSION_PROTOCOL  *This,
  IN CONST CHAR8                           *Name,
  IN UINT64                                Offset,
  IN UINTN                                 Size,
  OUT VOID                                 *Buffer
  )
{
  BeginOperation (Name);
  return FastbootRead (Name, Offset, Size, Buffer, FlashProgress);
}

STATIC EFI_STATUS EFIAPI
PlatformGetVarAll (
  IN FASTBOOT_PLATFORM_EXTENSION_PROTOCOL  *This,
  IN FASTBOOT_VARIABLE_CALLBACK            Callback,
  IN VOID                                  *Context
  )
{
  STATIC CONST CHAR8  *Names[] = { "product", "version-bootloader", "version-ec" };
  CHAR8               Value[61];
  EFI_STATUS          Status;
  UINTN               Index;

  for (Index = 0; Index < ARRAY_SIZE (Names); Index++) {
    Status = FastbootGetVar (Names[Index], Value, sizeof (Value));
    if (!EFI_ERROR (Status)) {
      Status = Callback (Context, Names[Index], Value);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  return EFI_SUCCESS;
}

STATIC FASTBOOT_PLATFORM_PROTOCOL            mPlatform = {
  PlatformInit,
  FastbootPlatformStop,
  PlatformFlash,
  PlatformErase,
  PlatformGetVar,
  PlatformOem
};
STATIC FASTBOOT_PLATFORM_EXTENSION_PROTOCOL  mExtension = {
  FASTBOOT_PLATFORM_EXTENSION_REVISION, PlatformRead, PlatformGetVarAll
};

EFI_STATUS EFIAPI
K3FastbootEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  Handle = NULL;

  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gGblEfiFastbootTransportProtocolGuid,
                &mTransport,
                &gAndroidFastbootPlatformProtocolGuid,
                &mPlatform,
                &gAndroidFastbootPlatformExtensionProtocolGuid,
                &mExtension,
                NULL
                );
}
