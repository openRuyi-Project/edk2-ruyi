// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <PiDxe.h>
#include <Protocol/LoadedImage.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FastbootPlatformLib.h>
#include <Library/FastbootTransportLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/HobLib.h>
#include <Pi/PiBootMode.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#define PACKET_BYTES         SIZE_16KB
#define DOWNLOAD_TIMEOUT_NS  5000000000ULL

STATIC EFI_STATUS
LoadRamImage (
  EFI_HANDLE  Parent,
  VOID        *Buffer,
  UINTN       Bytes,
  EFI_HANDLE  *Image
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *ParentImage;
  EFI_DEVICE_PATH_PROTOCOL   *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL   *FilePath;
  MEMMAP_DEVICE_PATH         Memory;
  EFI_STATUS                 Status;

  if (!Buffer || !Bytes || !Image ||
      (Bytes - 1 > MAX_UINTN - (UINTN)Buffer))
  {
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->HandleProtocol (
                  Parent,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&ParentImage
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DevicePath = DevicePathFromHandle (ParentImage->DeviceHandle);
  if (!DevicePath) {
    return EFI_NOT_FOUND;
  }

  ZeroMem (&Memory, sizeof (Memory));
  Memory.Header.Type    = HARDWARE_DEVICE_PATH;
  Memory.Header.SubType = HW_MEMMAP_DP;
  SetDevicePathNodeLength (&Memory.Header, sizeof (Memory));
  Memory.MemoryType      = EfiBootServicesData;
  Memory.StartingAddress = (UINTN)Buffer;
  Memory.EndingAddress   = (UINTN)Buffer + Bytes - 1;
  FilePath               = AppendDevicePathNode (DevicePath, &Memory.Header);
  if (!FilePath) {
    return EFI_OUT_OF_RESOURCES;
  }

  /* A real parent device plus the RAM file node lets Shell discover its
     image origin. DXE duplicates this path for the loaded image. */
  Status = gBS->LoadImage (FALSE, Parent, FilePath, Buffer, Bytes, Image);
  FreePool (FilePath);
  return Status;
}

STATIC EFI_STATUS
Reply (
  CONST CHAR8  *Prefix,
  CONST CHAR8  *Value
  )
{
  CHAR8  Response[65];

  AsciiSPrint (Response, sizeof (Response), "%a%a", Prefix, Value);
  return FastbootUsbSend (Response, AsciiStrLen (Response));
}

STATIC UINT64                  mLastProgress;
STATIC CONST CHAR8             *mFlashTarget;
STATIC BOOLEAN                 mProgressNow;
STATIC FASTBOOT_STORAGE_PHASE  mProgressPhase;
STATIC EFI_STATUS              mProgressUsbStatus;

STATIC EFI_STATUS
FlashProgress (
  UINT64                  Done,
  UINT64                  Total,
  FASTBOOT_STORAGE_PHASE  Phase
  )
{
  EFI_INPUT_KEY  Key;
  CHAR8          Value[60], SerialLine[128];
  UINT64         Now;
  CONST CHAR8    *Label, *Action;

  switch (Phase) {
    case FastbootStorageErase:
      Label  = "erase";
      Action = "erasing";
      break;
    case FastbootStorageWrite:
      Label  = "write";
      Action = "writing";
      break;
    case FastbootStorageVerify:
      Label  = "verify";
      Action = "verifying";
      break;
    case FastbootStorageRead:
      Label  = "read";
      Action = "reading";
      break;
    default:
      return EFI_INVALID_PARAMETER;
  }

  mProgressUsbStatus = FastbootUsbPoll ();

  if (EFI_ERROR (mProgressUsbStatus)) {
    return mProgressUsbStatus;
  }

  if (  !EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))
     && (Key.ScanCode == SCAN_ESC))
  {
    return EFI_ABORTED;
  }

  Now = GetPerformanceCounter ();
  if (  mProgressNow || (mProgressPhase != Phase) || (Done == Total)
     || (GetTimeInNanoSecond (Now - mLastProgress) >= 1000000000ULL))
  {
    UINT32  Percent = Done == Total ? 100 : (UINT32)MIN (
                                                      99,
                                                      (Done >> 10) * 100 / MAX (Total >> 10, 1)
                                                      );
    UINTN        Shift      = Total && Total < SIZE_1MB ? 10 : 20;
    CONST CHAR8  *Unit      = Shift == 10 ? "KiB" : "MiB";
    UINT64       TotalUnits = (Total >> Shift) + ((Total & ((1ULL << Shift) - 1)) != 0);
    UINT64       DoneUnits  = Done == Total ? TotalUnits : Done >> Shift;
    AsciiSPrint (
      SerialLine,
      sizeof (SerialLine),
      "\rFastboot %a %a: %Lu/%Lu %a (%u%%)    %a",
      mFlashTarget,
      Label,
      DoneUnits,
      TotalUnits,
      Unit,
      Percent,
      Done == Total ? "\r\n" : ""
      );
    SerialPortWrite ((UINT8 *)SerialLine, AsciiStrLen (SerialLine));
    AsciiSPrint (
      Value,
      sizeof (Value),
      "%a %Lu/%Lu %a",
      Action,
      DoneUnits,
      TotalUnits,
      Unit
      );
    mProgressUsbStatus = Reply ("INFO", Value);
    mProgressNow       = FALSE;
    mProgressPhase     = Phase;
    mLastProgress      = GetPerformanceCounter ();
  }

  return mProgressUsbStatus;
}

STATIC BOOLEAN
HexNumber (
  CONST CHAR8  *Text,
  UINTN        Digits,
  UINT64       *Value
  )
{
  *Value = 0;
  if ((Digits == 0) || (Digits > 16)) {
    return FALSE;
  }

  for (UINTN I = 0; I < Digits; I++) {
    CHAR8  C = Text[I];
    UINTN  N;
    if ((C >= '0') && (C <= '9')) {
      N = C - '0';
    } else if ((C >= 'a') && (C <= 'f')) {
      N = C - 'a' + 10;
    } else if ((C >= 'A') && (C <= 'F')) {
      N = C - 'A' + 10;
    } else {
      return FALSE;
    }

    *Value = (*Value << 4) | N;
  }

  return TRUE;
}

STATIC BOOLEAN
HexRange (
  CONST CHAR8  *Text,
  UINTN        Digits,
  UINT64       *Value
  )
{
  if ((Digits >= 2) && (Text[0] == '0') && ((Text[1] == 'x') || (Text[1] == 'X'))) {
    Text   += 2;
    Digits -= 2;
  }

  return HexNumber (Text, Digits, Value);
}

STATIC EFI_STATUS
Fetch (
  CHAR8  *Command,
  VOID   *Buffer
  )
{
  CHAR8       Name[48], Value[64], *OffsetText, *LengthText;
  UINT64      Capacity, Offset = 0, Length;
  EFI_STATUS  Status;

  OffsetText = AsciiStrStr (Command, ":");
  if (OffsetText != NULL) {
    *OffsetText++ = 0;
  }

  AsciiSPrint (Name, sizeof (Name), "partition-size:%a", Command);
  Status = FastbootGetVar (Name, Value, sizeof (Value));
  if (EFI_ERROR (Status)) {
    return Reply ("FAIL", "unknown partition");
  }

  Capacity = AsciiStrHexToUint64 (Value);
  Length   = Capacity;
  if (OffsetText != NULL) {
    LengthText = AsciiStrStr (OffsetText, ":");
    if (  (LengthText == NULL)
       || !HexRange (OffsetText, LengthText - OffsetText, &Offset)
       || !HexRange (
             LengthText + 1,
             AsciiStrLen (LengthText + 1),
             &Length
             ))
    {
      return Reply ("FAIL", "invalid read range");
    }
  }

  if (  (Length == 0) || (Length > FASTBOOT_DOWNLOAD_MAX) || (Offset > Capacity)
     || (Length > Capacity - Offset))
  {
    return Reply ("FAIL", "read range exceeds partition or buffer");
  }

  mFlashTarget       = Command;
  mProgressNow       = TRUE;
  mProgressUsbStatus = EFI_SUCCESS;
  Status             = FastbootRead (Command, Offset, (UINTN)Length, Buffer, FlashProgress);
  if (EFI_ERROR (mProgressUsbStatus)) {
    return mProgressUsbStatus;
  }

  if (EFI_ERROR (Status)) {
    return Reply ("FAIL", "partition read failed");
  }

  // INFO responses belong before DATA, never inside the binary stream.
  AsciiSPrint (Value, sizeof (Value), "%08x", (UINT32)Length);
  Status = Reply ("DATA", Value);
  for (UINTN Sent = 0; !EFI_ERROR (Status) && Sent < Length;) {
    UINTN  Bytes = MIN (PACKET_BYTES, (UINTN)Length - Sent);
    Status = FastbootUsbSend ((UINT8 *)Buffer + Sent, Bytes);
    Sent  += Bytes;
  }

  return EFI_ERROR (Status) ? Status : Reply ("OKAY", "");
}

STATIC EFI_STATUS
QuiesceUsb (
  VOID
  )
{
  EFI_STATUS  First   = FastbootUsbStop ();
  EFI_STATUS  Current = First;

  if (EFI_ERROR (First)) {
    Print (L"Fastboot USB halt failed; resources retained. Reset required.\r\n");
  }

  while (EFI_ERROR (Current)) {
    CpuDeadLoop ();
    // A debugger can release CpuDeadLoop; recheck DMA before cleanup.
    Current = FastbootUsbStop ();
  }

  return First;
}

STATIC EFI_STATUS
RestartUsb (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = QuiesceUsb ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = FastbootUsbStart ();
  if (!EFI_ERROR (Status)) {
    Print (L"USB Fastboot ready. Waiting for host commands.\r\n");
  }

  return Status;
}

EFI_STATUS EFIAPI
FastbootMain (
  EFI_HANDLE        ImageHandle,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS     Status, StopStatus;
  EFI_INPUT_KEY  Key;
  VOID           *Download = NULL;
  UINT8          *Packet = NULL;
  UINTN          Expected = 0, Done = 0, Size;
  UINT64         LastData = 0;
  BOOLEAN        Complete = FALSE, Exit = FALSE, Reboot = FALSE;
  CHAR8          Command[65], Value[64];

  // SerialPortLib state is private to this image, including RELEASE builds.
  SerialPortInitialize ();
  gST->ConOut->SetAttribute (gST->ConOut, EFI_WHITE | EFI_BACKGROUND_BLACK);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, TRUE);
  Print (
    GetBootModeHob () == BOOT_IN_RECOVERY_MODE ?
    L"RAM Recovery - USB Fastboot\r\n\r\n" : L"USB Fastboot\r\n\r\n"
    );
  Print (L"Initializing USB Fastboot...\r\n");
  Status = FastbootPlatformInit ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Download = AllocatePool (FASTBOOT_DOWNLOAD_MAX);
  Packet   = AllocatePool (PACKET_BYTES);
  if ((Download == NULL) || (Packet == NULL)) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Out;
  }

  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  Status = FastbootUsbStart ();
  if (EFI_ERROR (Status)) {
    goto Out;
  }

  Print (
    L"\r\nUSB Fastboot ready. Waiting for host commands.\r\n"
    L"Use fastboot devices to check the connection.\r\n"
    L"ESC or fastboot continue exits.\r\n"
    );
  while (!Exit) {
    if (  !EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))
       && (Key.ScanCode == SCAN_ESC))
    {
      Print (L"Fastboot: exiting on Esc.\r\n");
      Status = EFI_SUCCESS;
      break;
    }

    Size   = Expected ? MIN (Expected - Done, PACKET_BYTES) : 512;
    Status = FastbootUsbReceive (Packet, &Size);
    if (Status == EFI_MEDIA_CHANGED) {
      Expected = Done = 0;
      Complete = FALSE;
      continue;
    }

    if (Status == EFI_NOT_READY) {
      if (Expected && (GetTimeInNanoSecond (
                         GetPerformanceCounter () - LastData
                         ) >= DOWNLOAD_TIMEOUT_NS))
      {
        Expected = Done = 0;
        Complete = FALSE;
        Print (L"Download timed out; USB session restarted.\r\n");
        Status = RestartUsb ();
        if (EFI_ERROR (Status)) {
          break;
        }
      }

      MicroSecondDelay (20);
      continue;
    }

    if (EFI_ERROR (Status)) {
      goto Reconnect;
    }

    if (Expected) {
      if (Size == 0) {
        continue;
      }

      if (Size > Expected - Done) {
        Expected = 0;
        Complete = FALSE;
        Status   = Reply ("FAIL", "invalid data size");
      } else {
        CopyMem ((UINT8 *)Download + Done, Packet, Size);
        Done    += Size;
        LastData = GetPerformanceCounter ();
        if (Done == Expected) {
          Expected = 0;
          Complete = TRUE;
          Status   = Reply ("OKAY", "");
        }
      }

      if (EFI_ERROR (Status)) {
        goto Reconnect;
      }

      continue;
    }

    if (!Size || (Size > 64) || (ScanMem8 (Packet, Size, 0) != NULL)) {
      Status = Reply ("FAIL", "invalid command");
      if (EFI_ERROR (Status)) {
        goto Reconnect;
      }

      continue;
    }

    CopyMem (Command, Packet, Size);
    Command[Size] = 0;
    if (!AsciiStrnCmp (Command, "download:", 9)) {
      UINT64  Bytes;
      Complete = FALSE;
      Done     = 0;
      if (  (Size != 17) || !HexNumber (Command + 9, 8, &Bytes) || (Bytes == 0)
         || (Bytes > FASTBOOT_DOWNLOAD_MAX))
      {
        Status = Reply ("FAIL", "invalid download size");
      } else {
        Expected = (UINTN)Bytes;
        LastData = GetPerformanceCounter ();
        AsciiSPrint (Value, sizeof (Value), "%08x", (UINT32)Bytes);
        Status = Reply ("DATA", Value);
      }
    } else if (!AsciiStrnCmp (Command, "getvar:", 7)) {
      CONST CHAR8  *Name = Command + 7;
      if (!AsciiStrCmp (Name, "version")) {
        Status = Reply ("OKAY", "0.4");
      } else if (!AsciiStrCmp (Name, "is-userspace")) {
        Status = Reply ("OKAY", "no");
      } else if (  !AsciiStrCmp (Name, "max-download-size")
                || !AsciiStrCmp (Name, "max-fetch-size"))
      {
        AsciiSPrint (
          Value,
          sizeof (Value),
          "0x%08x",
          FASTBOOT_DOWNLOAD_MAX
          );
        Status = Reply ("OKAY", Value);
      } else if (!AsciiStrCmp (Name, "all")) {
        CONST CHAR8  *Variables[] = { "product", "version-bootloader", "version-ec" };
        Status = EFI_SUCCESS;
        for (UINTN I = 0; !EFI_ERROR (Status) && I < ARRAY_SIZE (Variables); I++) {
          if (!EFI_ERROR (FastbootGetVar (Variables[I], Value, sizeof (Value)))) {
            CHAR8  Info[64];
            AsciiSPrint (Info, sizeof (Info), "%a: %a", Variables[I], Value);
            Status = Reply ("INFO", Info);
          }
        }

        if (!EFI_ERROR (Status)) {
          Status = Reply ("OKAY", "");
        }
      } else {
        Status = FastbootGetVar (Name, Value, sizeof (Value));
        if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
          AsciiSPrint (Value, sizeof (Value), "variable unavailable: %r", Status);
        }

        Status = Reply (
                   EFI_ERROR (Status) ? "FAIL" : "OKAY",
                   Status == EFI_NOT_FOUND ? "unknown variable" : Value
                   );
      }
    } else if (!AsciiStrnCmp (Command, "flash:", 6)) {
      if (!Complete) {
        Status = Reply ("FAIL", "no complete download");
        if (EFI_ERROR (Status)) {
          goto Reconnect;
        }

        continue;
      }

      mProgressNow       = TRUE;
      mFlashTarget       = Command + 6;
      mProgressUsbStatus = EFI_SUCCESS;
      Status             = FastbootFlash (Command + 6, Download, Done, FlashProgress);
      Complete           = FALSE;
      Done               = 0;
      CHAR8  Result[96];
      AsciiSPrint (
        Result,
        sizeof (Result),
        "\r\nFastboot %a: %r\r\n",
        mFlashTarget,
        Status
        );
      SerialPortWrite ((UINT8 *)Result, AsciiStrLen (Result));
      // Storage failures and local cancellation can still be reported over
      // the current connection. Only a transport failure needs recovery.
      if (EFI_ERROR (mProgressUsbStatus)) {
        Status = mProgressUsbStatus;
        goto Reconnect;
      }

      if (!EFI_ERROR (Status) && !AsciiStrCmp (mFlashTarget, "ec")) {
        Status = Reply ("INFO", "EC verified; use oem reboot-ec to activate");
        if (EFI_ERROR (Status)) {
          goto Reconnect;
        }
      }

      AsciiSPrint (Value, sizeof (Value), "%r", Status);
      Status = Reply (
                 EFI_ERROR (Status) ? "FAIL" : "OKAY",
                 EFI_ERROR (Status) ? Value : ""
                 );
    } else if (!AsciiStrnCmp (Command, "fetch:", 6)) {
      Complete = FALSE;
      Status   = Fetch (Command + 6, Download);
    } else if (!AsciiStrCmp (Command, "oem reboot-ec")) {
      Complete = FALSE;
      Status   = FastbootRebootEc ();
      Status   = Reply (
                   EFI_ERROR (Status) ? "FAIL" : "OKAY",
                   EFI_ERROR (Status) ? "EC reboot failed" : "Wait 15 seconds, then reboot the board"
                   );
    } else if (!AsciiStrCmp (Command, "reboot")) {
      Status = Reply ("OKAY", "");
      Reboot = !EFI_ERROR (Status);
      Exit   = Reboot;
    } else if (!AsciiStrCmp (Command, "continue")) {
      Print (L"Fastboot: continuing at host request.\r\n");
      Status = Reply ("OKAY", "");
      Exit   = !EFI_ERROR (Status);
    } else if (  !AsciiStrCmp (Command, "boot")
              || !AsciiStrCmp (Command, "oem boot-efi"))
    {
      EFI_HANDLE  Loaded = NULL;
      Status = Complete ? LoadRamImage (
                            ImageHandle,
                            Download,
                            Done,
                            &Loaded
                            )
                        : EFI_NOT_READY;
      if (EFI_ERROR (Status)) {
        Status = Reply ("FAIL", "no loadable EFI image");
      } else {
        Status = Reply ("OKAY", "");
        if (!EFI_ERROR (Status)) {
          Status = QuiesceUsb ();
          if (EFI_ERROR (Status)) {
            gBS->UnloadImage (Loaded);
            Loaded = NULL;
            Print (L"Fastboot handoff canceled after the USB halt failure.\r\n");
            goto Out;
          } else {
            EFI_STATUS  ImageStatus = gBS->StartImage (Loaded, NULL, NULL);

            // DXE unloads returned applications and images returning errors.
            Loaded   = NULL;
            Complete = FALSE;
            Expected = Done = 0;
            Print (L"EFI application returned: %r; restarting USB Fastboot.\r\n", ImageStatus);
            Status = FastbootUsbStart ();
          }
        }

        if (Loaded != NULL) {
          gBS->UnloadImage (Loaded);
        }
      }
    } else {
      Status = Reply ("FAIL", "unsupported command");
    }

    if (!EFI_ERROR (Status)) {
      continue;
    }

Reconnect:
    Expected = Done = 0;
    Complete = FALSE;
    // The transport has already discarded the old endpoints on bus reset
    // or disconnect. Let the host enumerate without resetting it again.
    if (Status == EFI_MEDIA_CHANGED) {
      continue;
    }

    Print (L"Fastboot transport error: %r; restarting USB session.\r\n", Status);
    Status = RestartUsb ();
    if (EFI_ERROR (Status)) {
      break;
    }
  }

Out:
  StopStatus = QuiesceUsb ();
  if (EFI_ERROR (StopStatus) && !EFI_ERROR (Status)) {
    Status = StopStatus;
  }

  FastbootPlatformStop ();
  if (Download != NULL) {
    FreePool (Download);
  }

  if (Packet != NULL) {
    FreePool (Packet);
  }

  if (Reboot && !EFI_ERROR (Status)) {
    Print (L"Fastboot: rebooting through UEFI ResetSystem.\r\n");
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
    Status = EFI_DEVICE_ERROR;
  }

  if (EFI_ERROR (Status)) {
    Print (L"Fastboot stopped: %r\r\n", Status);
  }

  return Status;
}
