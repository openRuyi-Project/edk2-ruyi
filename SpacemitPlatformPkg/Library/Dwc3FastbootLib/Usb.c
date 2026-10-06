/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/FastbootTransportLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiLib.h>
#include <Library/UsbDevicePlatformLib.h>
#include "Dwc3.h"

typedef enum {
  Setup, DataIn, WaitStatusIn, WaitStatusOut, StatusIn, StatusOut
} CONTROL_STATE;
STATIC UINTN          mBase;
STATIC UINT8          *mDma;
STATIC DWC3_TRB       *mTrb;
STATIC UINT8          *mSetup, *mControl, *mRx, *mTx;
STATIC UINT32         mEventOffset, mGeneration, mSeenGeneration;
STATIC UINT8          mResource[4], mConfiguration;
STATIC BOOLEAN        mStarted, mRxReady, mTxBusy;
STATIC UINTN          mRxBytes;
STATIC UINTN          mRxTransferBytes;
STATIC UINT16         mBulkPacket;
STATIC EFI_STATUS     mError;
STATIC CONTROL_STATE  mControlState;
// Separate pages avoid invalidating unrelated CPU-owned data.
#define DMA_PAGES            12
#define COMMAND_TIMEOUT_US   1000
#define HALT_TIMEOUT_US      100000
#define TRANSFER_TIMEOUT_NS  2000000000ULL

STATIC CONST UINT8  mDevice[] = {
  18, 1, 0, 2, 0, 0, 0, 64, 0x1c, 0x36, 1, 0x10, 0, 1, 1, 2, 0, 1
};
STATIC CONST UINT8  mConfig[] = {
  9, 2, 32,   0, 1, 1,    0,    0x80, 50,
  9, 4, 0,    0, 2, 0xff, 0x42, 3,    0,
  7, 5, 1,    2, 0, 2,    0,
  7, 5, 0x81, 2, 0, 2,    0
};
STATIC CONST UINT8  mQualifier[] = { 10, 6, 0, 2, 0, 0, 0, 64, 1, 0 };

STATIC UINT32
Read (
  UINT32  Offset
  )
{
  return MmioRead32 (mBase + Offset);
}

STATIC VOID
Write (
  UINT32  Offset,
  UINT32  Value
  )
{
  MmioWrite32 (mBase + Offset, Value);
}

STATIC EFI_STATUS
Wait (
  UINT32  Offset,
  UINT32  Mask,
  UINT32  Value,
  UINTN   Usec
  )
{
  UINT64  Start = GetPerformanceCounter ();

  do {
    if ((Read (Offset) & Mask) == Value) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  } while (GetTimeInNanoSecond (GetPerformanceCounter () - Start) < Usec * 1000ULL);

  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
Command (
  UINTN   Ep,
  UINT32  Cmd,
  UINT32  P0,
  UINT32  P1,
  UINT32  P2
  )
{
  EFI_STATUS  Status;

  Write (DEPCMDPAR2 (Ep), P2);
  Write (DEPCMDPAR1 (Ep), P1);
  Write (DEPCMDPAR0 (Ep), P0);
  Write (DEPCMD (Ep), Cmd | CMD_ACTIVE);
  Status = Wait (DEPCMD (Ep), CMD_ACTIVE, 0, COMMAND_TIMEOUT_US);
  if (!EFI_ERROR (Status) && (Read (DEPCMD (Ep)) & CMD_STATUS)) {
    Status = EFI_DEVICE_ERROR;
  }

  if (EFI_ERROR (Status)) {
    Print (
      L"DWC3 command failed: ep=%u cmd=%x result=%08x status=%r\r\n",
      Ep,
      Cmd,
      Read (DEPCMD (Ep)),
      Status
      );
  }

  return Status;
}

STATIC EFI_STATUS
Configure (
  UINTN    Ep,
  UINTN    Packet,
  BOOLEAN  Modify
  )
{
  UINT32      P0 = (UINT32)Packet << 3;
  EFI_STATUS  Status;

  if (Ep >= 2) {
    // Bulk endpoint type.
    P0 |= 2U << 1;
  }

  if (Ep & 1) {
    P0 |= (UINT32)(Ep / 2) << 17;
  }

  if (Modify) {
    P0 |= 2U << 30;
  }

  Status = Command (Ep, CMD_CONFIG, P0, ((UINT32)Ep << 25) | BIT8 | BIT10, 0);
  if (EFI_ERROR (Status) || Modify) {
    return Status;
  }

  return Command (Ep, CMD_RESOURCE, 1, 0, 0);
}

STATIC EFI_STATUS
Start (
  UINTN  Ep,
  VOID   *Buffer,
  UINTN  Size,
  UINTN  Type
  )
{
  DWC3_TRB    *Trb = &mTrb[Ep];
  EFI_STATUS  Status;
  UINT64      Address = (UINTN)Buffer, TrbAddress = (UINTN)Trb;

  if (mResource[Ep]) {
    return EFI_ALREADY_STARTED;
  }

  if (Size != 0) {
    if (Ep & 1) {
      WriteBackDataCacheRange (Buffer, Size);
    } else {
      WriteBackInvalidateDataCacheRange (Buffer, Size);
    }
  }

  Trb->Low     = (UINT32)Address;
  Trb->High    = (UINT32)(Address >> 32);
  Trb->Size    = (UINT32)Size;
  Trb->Control = TRB_HWO | TRB_LAST | TRB_ISP | TRB_IOC | ((UINT32)Type << 4);
  WriteBackDataCacheRange (Trb, sizeof (*Trb));
  MemoryFence ();
  Status = Command (Ep, CMD_START, (UINT32)(TrbAddress >> 32), (UINT32)TrbAddress, 0);
  if (!EFI_ERROR (Status)) {
    mResource[Ep] = (UINT8)((Read (DEPCMD (Ep)) >> 16) & 0x7f);
  }

  return Status;
}

STATIC EFI_STATUS
End (
  UINTN  Ep
  )
{
  EFI_STATUS  Status = EFI_SUCCESS;

  if (mResource[Ep]) {
    Status = Command (Ep, CMD_END | BIT11 | ((UINT32)mResource[Ep] << 16), 0, 0, 0);
    if (!EFI_ERROR (Status)) {
      mResource[Ep] = 0;
    }
  }

  return Status;
}

STATIC EFI_STATUS
ArmSetup (
  VOID
  )
{
  mControlState = Setup;
  return Start (0, mSetup, 8, TRB_SETUP);
}

STATIC EFI_STATUS
StallControl (
  VOID
  )
{
  EFI_STATUS  Status = End (1);

  if (!EFI_ERROR (Status)) {
    Status = End (0);
  }

  if (!EFI_ERROR (Status)) {
    Status = Command (0, CMD_STALL, 0, 0, 0);
  }

  return EFI_ERROR (Status) ? Status : ArmSetup ();
}

STATIC EFI_STATUS
SetConfiguration (
  UINT8  Configuration
  )
{
  EFI_STATUS  Status;

  for (UINTN Ep = 2; Ep < 4; Ep++) {
    Status = End (Ep);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Write (DALEPENA, 3);
  mConfiguration = 0;
  mRxReady       = mTxBusy = FALSE;
  mGeneration++;
  if (!Configuration) {
    return EFI_SUCCESS;
  }

  Status = Command (0, CMD_NEW | (2U << 16), 0, 0, 0);
  for (UINTN Ep = 2; !EFI_ERROR (Status) && Ep < 4; Ep++) {
    Status = Configure (Ep, mBulkPacket, FALSE);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Write (DALEPENA, 15);
  mConfiguration = Configuration;
  // Arm OUT only when the consumer provides its next receive length.
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
SetupRequest (
  VOID
  )
{
  UINT8       RequestType, Request;
  UINT16      Value, Index, Length;
  CONST VOID  *Data = NULL;
  UINTN       Bytes = 0;
  EFI_STATUS  Status;

  InvalidateDataCacheRange (mSetup, 64);
  MemoryFence ();
  RequestType = mSetup[0];
  Request     = mSetup[1];
  Value       = ReadUnaligned16 ((UINT16 *)(mSetup + 2));
  Index       = ReadUnaligned16 ((UINT16 *)(mSetup + 4));
  Length      = ReadUnaligned16 ((UINT16 *)(mSetup + 6));
  if ((RequestType == 0x80) && (Request == 6)) {
    switch (Value >> 8) {
      case 1: if ((Value & 0xff) == 0) {
          Data  = mDevice;
          Bytes = sizeof (mDevice);
      }

        break;
      case 2:
      case 7:
        if ((Value & 0xff) == 0) {
          UINT16  Packet = (Value >> 8) == 2 ? mBulkPacket : (mBulkPacket == 512 ? 64 : 512);
          CopyMem (mControl, mConfig, sizeof (mConfig));
          mControl[1] = (UINT8)(Value >> 8);
          WriteUnaligned16 ((UINT16 *)(mControl + 22), Packet);
          WriteUnaligned16 ((UINT16 *)(mControl + 29), Packet);
          Data  = mControl;
          Bytes = sizeof (mConfig);
        }

        break;
      case 6: if ((Value & 0xff) == 0) {
          Data  = mQualifier;
          Bytes = sizeof (mQualifier);
      }

        break;
      case 3:
        if ((Value & 0xff) == 0) {
          mControl[0] = 4;
          mControl[1] = 3;
          mControl[2] = 9;
          mControl[3] = 4;
          Data        = mControl;
          Bytes       = 4;
        } else if (((Value & 0xff) <= 2) && (Index == 0x409)) {
          CONST CHAR8  *Text = (Value & 0xff) == 1 ? "SpacemiT" : "K3 UEFI Fastboot";
          Bytes       = 2 + 2 * AsciiStrLen (Text);
          mControl[0] = (UINT8)Bytes;
          mControl[1] = 3;
          for (UINTN I = 0; Text[I]; I++) {
            mControl[2 + 2 * I] = Text[I];
            mControl[3 + 2 * I] = 0;
          }

          Data = mControl;
        }

        break;
      default: break;
    }
  } else if ((RequestType == 0) && (Request == 5) && (Value <= 127) && !Index && !Length) {
    Write (DCFG, (Read (DCFG) & ~(0x7fU << 3)) | ((UINT32)Value << 3));
    mControlState = WaitStatusIn;
    return EFI_SUCCESS;
  } else if ((RequestType == 0) && (Request == 9) && (Value <= 1) && !Index && !Length) {
    Status = SetConfiguration ((UINT8)Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    mControlState = WaitStatusIn;
    return EFI_SUCCESS;
  } else if ((RequestType == 0x80) && (Request == 8) && !Value && !Index && (Length == 1)) {
    mControl[0] = mConfiguration;
    Data        = mControl;
    Bytes       = 1;
  } else if (((RequestType == 0x80) || (RequestType == 0x81) || (RequestType == 0x82)) &&
             (Request == 0) && !Value && (Length == 2))
  {
    mControl[0] = mControl[1] = 0;
    Data        = mControl;
    Bytes       = 2;
  } else if ((RequestType == 0x81) && (Request == 10) && !Value && !Index && (Length == 1)) {
    mControl[0] = 0;
    Data        = mControl;
    Bytes       = 1;
  } else if ((RequestType == 1) && (Request == 11) && !Value && !Index && !Length) {
    mControlState = WaitStatusIn;
    return EFI_SUCCESS;
  } else if ((RequestType == 2) && (Request == 1) && !Value && !Length &&
             ((Index == 1) || (Index == 0x81)))
  {
    Status        = Command (Index == 1 ? 2 : 3, CMD_UNSTALL, 0, 0, 0);
    mControlState = WaitStatusIn;
    return Status;
  }

  if ((Data == NULL) || !Length) {
    return StallControl ();
  }

  Bytes = MIN (Bytes, Length);
  if (Data != mControl) {
    CopyMem (mControl, Data, Bytes);
  }

  mControlState = DataIn;
  return Start (1, mControl, Bytes, TRB_DATA);
}

STATIC EFI_STATUS
Event (
  UINT32  Value
  )
{
  EFI_STATUS  Status;

  if (Value & 1) {
    UINTN  Type = (Value >> 8) & 0xf;
    if ((Type == 0) || (Type == 1)) {
      // Disconnect or USB reset invalidates a download.
      // DWC3 keeps EP0's setup transfer across bus reset. Stopping and
      // immediately restarting it here races the controller reset phase.
      // Non-control transfers belong to the configuration being discarded.
      for (UINTN Ep = 2; Ep < 4; Ep++) {
        Status = End (Ep);
        if (EFI_ERROR (Status)) {
          return Status;
        }
      }

      mConfiguration = 0;
      mRxReady       = mTxBusy = FALSE;
      mGeneration++;
      Write (DALEPENA, 3);
      Write (DCFG, Read (DCFG) & ~(0x7fU << 3));
      return EFI_SUCCESS;
    }

    if (Type == 2) {
      // USB 2.0 transport supports high- and full-speed negotiation.
      UINT32  Speed = Read (DSTS) & 7;
      if ((Speed != 0) && (Speed != 1) && (Speed != 3)) {
        return EFI_UNSUPPORTED;
      }

      mBulkPacket = Speed == 0 ? 512 : 64;
      Status      = Configure (0, 64, TRUE);
      return EFI_ERROR (Status) ? Status : Configure (1, 64, TRUE);
    }

    return Type == 9 ? EFI_DEVICE_ERROR : EFI_SUCCESS;
  }

  UINTN  Ep = (Value >> 1) & 0x1f, Type = (Value >> 6) & 0xf;

  if (Ep >= 4) {
    return EFI_SUCCESS;
  }

  if (Type == 1) {
    // Transfer complete.
    mResource[Ep] = 0;
    InvalidateDataCacheRange (&mTrb[Ep], sizeof (mTrb[Ep]));
    MemoryFence ();
    if ((Ep == 0) && (mControlState == Setup)) {
      return SetupRequest ();
    }

    if ((Ep == 1) && (mControlState == DataIn)) {
      mControlState = WaitStatusOut;
    } else if (((Ep == 0) && (mControlState == StatusOut)) ||
               ((Ep == 1) && (mControlState == StatusIn)))
    {
      return ArmSetup ();
    } else if (Ep == 2) {
      UINT32  Residual = mTrb[2].Size & 0xffffff;
      if (Residual > mRxTransferBytes) {
        return EFI_DEVICE_ERROR;
      }

      mRxBytes = mRxTransferBytes - Residual;
      InvalidateDataCacheRange (mRx, BULK_BYTES);
      MemoryFence ();
      mRxReady = TRUE;
    } else if (Ep == 3) {
      mTxBusy = FALSE;
    }
  } else if (Type == 3) {
    // Transfer not ready: host entered the status phase.
    if ((Ep == 1) && (mControlState == WaitStatusIn)) {
      mControlState = StatusIn;
      return Start (1, mControl, 0, TRB_STATUS2);
    }

    if ((Ep == 0) && (mControlState == WaitStatusOut)) {
      mControlState = StatusOut;
      return Start (0, mControl, 0, TRB_STATUS3);
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
FastbootUsbPoll (
  VOID
  )
{
  if (!mStarted) {
    return EFI_NOT_STARTED;
  }

  if (EFI_ERROR (mError)) {
    return mError;
  }

  // Bound each call even if the host continuously produces events.
  for (UINTN I = 0; I < EVENT_BYTES / 4; I++) {
    UINT32  Count = Read (GEVNTCOUNT) & 0xffff;
    if (!Count) {
      break;
    }

    if ((Count > EVENT_BYTES) || (Count & 3)) {
      Print (L"DWC3 invalid event count: %08x\r\n", Count);
      mError = EFI_DEVICE_ERROR;
      break;
    }

    InvalidateDataCacheRange (mDma + mEventOffset, 4);
    MemoryFence ();
    UINT32  Value = ReadUnaligned32 ((UINT32 *)(mDma + mEventOffset));
    mEventOffset = (mEventOffset + 4) % EVENT_BYTES;
    Write (GEVNTCOUNT, 4);
    mError = Event (Value);
    if (EFI_ERROR (mError)) {
      Print (
        L"DWC3 event failed: event=%08x control=%u DSTS=%08x status=%r\r\n",
        Value,
        mControlState,
        Read (DSTS),
        mError
        );
      break;
    }
  }

  if (!EFI_ERROR (mError) && (mSeenGeneration != mGeneration)) {
    mSeenGeneration = mGeneration;
    return EFI_MEDIA_CHANGED;
  }

  return mError;
}

EFI_STATUS
FastbootUsbStop (
  VOID
  )
{
  if (mDma == NULL) {
    return EFI_SUCCESS;
  }

  // A host that stopped reading can leave bulk IN active indefinitely.
  // End transfers while the endpoint command interface is still running.
  for (UINTN Ep = 0; Ep < ARRAY_SIZE (mResource); Ep++) {
    End (Ep);
  }

  Write (DCTL, Read (DCTL) & ~RUN_STOP);
  if (EFI_ERROR (Wait (DSTS, CORE_HALTED, CORE_HALTED, HALT_TIMEOUT_US))) {
    mError   = EFI_TIMEOUT;
    mStarted = FALSE;
    Print (L"DWC3 did not halt; DMA allocation retained.\r\n");
    return EFI_TIMEOUT;
  }

  Write (DEVTEN, 0);
  Write (GEVNTSIZ, BIT31);
  Write (GEVNTCOUNT, Read (GEVNTCOUNT) & 0xffff);
  Write (GEVNTADRLO, 0);
  Write (GEVNTADRHI, 0);
  FreePages (mDma, DMA_PAGES);
  mDma     = NULL;
  mStarted = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS
FastbootUsbStart (
  VOID
  )
{
  EFI_STATUS  Status;

  if (mDma != NULL) {
    return EFI_ALREADY_STARTED;
  }

  Status = UsbDevicePlatformInit (&mBase);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Read (GSNPSID) & 0xffff0000U) != 0x55330000U) {
    return EFI_UNSUPPORTED;
  }

  Write (DCTL, (Read (DCTL) & ~RUN_STOP) | CORE_RESET);
  Status = Wait (DCTL, CORE_RESET, 0, HALT_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mDma = AllocatePages (DMA_PAGES);
  if (mDma == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (mDma, DMA_PAGES * EFI_PAGE_SIZE);
  mTrb     = (VOID *)(mDma + EFI_PAGE_SIZE);
  mSetup   = mDma + 2 * EFI_PAGE_SIZE;
  mControl = mDma + 3 * EFI_PAGE_SIZE;
  mRx      = mDma + 4 * EFI_PAGE_SIZE;
  mTx      = mDma + 8 * EFI_PAGE_SIZE;
  ZeroMem (mResource, sizeof (mResource));
  mEventOffset   = 0;
  mConfiguration = 0;
  mRxReady       = mTxBusy = FALSE;
  mBulkPacket    = 512;
  mGeneration++;
  mSeenGeneration = mGeneration;
  mError          = EFI_SUCCESS;
  WriteBackInvalidateDataCacheRange (mDma, DMA_PAGES * EFI_PAGE_SIZE);
  Write (GCTL, (Read (GCTL) & ~(3U << 12)) | (2U << 12));
  // Retained K3 DT requires these PHY suspend/low-power quirks.
  Write (GUSB2PHYCFG, Read (GUSB2PHYCFG) & ~(BIT6 | BIT8));
  Write (GUSB3PIPECTL, Read (GUSB3PIPECTL) & ~BIT17);
  Write (DCFG, Read (DCFG) & ~((0x7fU << 3) | 7U | BIT22));
  Write (GEVNTADRLO, (UINT32)(UINTN)mDma);
  Write (GEVNTADRHI, (UINT32)((UINT64)(UINTN)mDma >> 32));
  Write (GEVNTSIZ, EVENT_BYTES | BIT31);
  Write (GEVNTCOUNT, Read (GEVNTCOUNT) & 0xffff);
  Write (DEVTEN, BIT0 | BIT1 | BIT2 | BIT9);
  Status = Command (0, CMD_NEW, 0, 0, 0);
  for (UINTN Ep = 0; !EFI_ERROR (Status) && Ep < 2; Ep++) {
    Status = Configure (Ep, 64, FALSE);
  }

  Write (DALEPENA, 3);
  if (!EFI_ERROR (Status)) {
    Status = ArmSetup ();
  }

  if (!EFI_ERROR (Status)) {
    mStarted = TRUE;
    Write (DCTL, Read (DCTL) | RUN_STOP);
    Status = Wait (DSTS, CORE_HALTED, 0, HALT_TIMEOUT_US);
  }

  if (EFI_ERROR (Status)) {
    FastbootUsbStop ();
  }

  return Status;
}

EFI_STATUS
FastbootUsbReceive (
  VOID   *Buffer,
  UINTN  *Length
  )
{
  EFI_STATUS  Status = FastbootUsbPoll ();

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!mConfiguration) {
    return EFI_NOT_READY;
  }

  if (!mRxReady) {
    if (!mResource[2]) {
      if (!*Length || (*Length > BULK_BYTES)) {
        return EFI_INVALID_PARAMETER;
      }

      // DWC3 OUT lengths must be whole packets. A short final USB packet
      // completes an odd-sized tail; a packet-aligned tail completes at the
      // requested length without requiring an extra host zero-length packet.
      mRxTransferBytes = (*Length + mBulkPacket - 1) & ~(UINTN)(mBulkPacket - 1);
      Status           = Start (2, mRx, mRxTransferBytes, TRB_NORMAL);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }

    return EFI_NOT_READY;
  }

  if (*Length < mRxBytes) {
    *Length = mRxBytes;
    return EFI_BUFFER_TOO_SMALL;
  }

  CopyMem (Buffer, mRx, mRxBytes);
  *Length  = mRxBytes;
  mRxReady = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS
FastbootUsbSend (
  CONST VOID  *Buffer,
  UINTN       Length
  )
{
  EFI_STATUS  Status;
  UINT32      Generation = mGeneration;
  UINT64      Begin;

  if (!mStarted || !mConfiguration) {
    return EFI_NOT_READY;
  }

  if ((Length > BULK_BYTES) || mTxBusy) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (mTx, Buffer, Length);
  mTxBusy = TRUE;
  Status  = Start (3, mTx, Length, TRB_NORMAL);
  if (EFI_ERROR (Status)) {
    mTxBusy = FALSE;
    return Status;
  }

  Begin = GetPerformanceCounter ();
  while (mTxBusy) {
    Status = FastbootUsbPoll ();
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Generation != mGeneration) {
      return EFI_MEDIA_CHANGED;
    }

    if (GetTimeInNanoSecond (GetPerformanceCounter () - Begin) >= TRANSFER_TIMEOUT_NS) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (10);
  }

  return EFI_SUCCESS;
}
