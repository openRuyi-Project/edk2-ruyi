/**
 *
 *  Copyright (c) 2025, SpacemiT Co., Ltd. All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/TimerLib.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/K3InnoDpLib.h>
#include "InnoDpReg.h"

#include "DpErrors.h"

#define DP_AUX_I2C_WRITE     0x0
#define DP_AUX_I2C_READ      0x1
#define DP_AUX_I2C_MOT       0x4
#define DP_AUX_NATIVE_WRITE  0x8
#define DP_AUX_NATIVE_READ   0x9

#define DP_AUX_NATIVE_REPLY_ACK    0x00
#define DP_AUX_NATIVE_REPLY_NACK   0x01
#define DP_AUX_NATIVE_REPLY_DEFER  0x02

#define DP_DPCD_REV             0x000
#define DP_MAX_LINK_RATE        0x001
#define DP_MAX_LANE_COUNT       0x002
#define DP_MAX_LANE_COUNT_MASK  0x1f
#define DP_TPS3_SUPPORTED       0x40
#define DP_ENHANCED_FRAME_CAP   0x80

#define DP_LINK_BW_SET   0x100
#define DP_LINK_BW_1_62  0x06
#define DP_LINK_BW_2_7   0x0a
#define DP_LINK_BW_5_4   0x14
#define DP_LINK_BW_8_1   0x1e

#define DP_LANE_COUNT_SET                0x101
#define DP_LANE_COUNT_ENHANCED_FRAME_EN  0x80

#define DP_TRAINING_PATTERN_SET      0x102
#define DP_TRAINING_PATTERN_DISABLE  0
#define DP_TRAINING_PATTERN_1        1
#define DP_TRAINING_PATTERN_2        2
#define DP_TRAINING_PATTERN_3        3
#define DP_LINK_SCRAMBLING_DISABLE   0x20

#define DP_TRAINING_LANE0_SET              0x103
#define DP_TRAIN_VOLTAGE_SWING_MASK        0x3
#define DP_TRAIN_PRE_EMPHASIS_MASK         (3 << 3)
#define DP_TRAIN_PRE_EMPHASIS_SHIFT        3
#define DP_TRAIN_MAX_SWING_REACHED         0x04
#define DP_TRAIN_MAX_PRE_EMPHASIS_REACHED  0x20

#define DP_DOWNSPREAD_CTRL  0x107

#define DP_MAIN_LINK_CHANNEL_CODING_SET  0x108
#define DP_SET_ANSI_8B10B                0x01

#define DP_TRAINING_AUX_RD_INTERVAL  0x00e
#define DP_TRAINING_AUX_RD_MASK      0x7f

#define DP_LANE0_1_STATUS        0x202
#define DP_LANE_CR_DONE          0x01
#define DP_LANE_CHANNEL_EQ_DONE  0x02
#define DP_LANE_SYMBOL_LOCKED    0x04

#define DP_EDP_CONFIGURATION_SET             0x10a
#define DP_ALTERNATE_SCRAMBLER_RESET_ENABLE  0x01
#define DP_FRAMING_CHANGE_ENABLE             0x02
#define DP_PANEL_SELF_TEST_ENABLE            0x80

#define DP_ADJUST_REQUEST_LANE0_1  0x206

#define DP_SET_POWER       0x600
#define DP_SET_POWER_D0    0x1
#define DP_SET_POWER_D3    0x2
#define DP_SET_POWER_MASK  0x7

#define DP_LINK_STATUS_SIZE  6

#define SOC_DP_SWING_MAX   3
#define SOC_DP_PREEMP_MAX  3

static const SOC_DP_LINK_CONFIG  SocDpLinkPriorityTable[] = {
  /* --- Tier 1: Low Bandwidth (< 4 Gbps) --- */
  { SOC_DP_LINK_RATE_1_62, SOC_DP_LANE_1 },  /* 1.62 Gbps */
  { SOC_DP_LINK_RATE_2_70, SOC_DP_LANE_1 },  /* 2.70 Gbps */
  { SOC_DP_LINK_RATE_1_62, SOC_DP_LANE_2 },  /* 3.24 Gbps */

  /* --- Tier 2: Medium Bandwidth (~5-6 Gbps) --- */
  { SOC_DP_LINK_RATE_2_70, SOC_DP_LANE_2 },  /* 5.40 Gbps */
  { SOC_DP_LINK_RATE_1_62, SOC_DP_LANE_4 },  /* 6.48 Gbps */

  /* --- Tier 3: High Bandwidth (~10 Gbps) --- */
  { SOC_DP_LINK_RATE_2_70, SOC_DP_LANE_4 },  /* 10.8 Gbps */
  { SOC_DP_LINK_RATE_5_40, SOC_DP_LANE_2 },  /* 10.8 Gbps */

  /* --- Tier 4: Ultra High Bandwidth (> 17 Gbps) --- */
  { SOC_DP_LINK_RATE_5_40, SOC_DP_LANE_4 },  /* 21.6 Gbps */
};

static const struct SocFormatInfo {
  UINT8    Bpp;
} FormatInfoTable[] = {
  [SOC_VIDEO_RGB_6BIT]  =    { .Bpp = 18 },
  [SOC_VIDEO_RGB_8BIT]  =    { .Bpp = 24 },
  [SOC_VIDEO_RGB_10BIT] =    { .Bpp = 30 },
  [SOC_VIDEO_RGB_12BIT] =    { .Bpp = 36 },
  [SOC_VIDEO_RGB_16BIT] =    { .Bpp = 48 },

  [SOC_VIDEO_YUV444_8BIT]  = { .Bpp = 24 },
  [SOC_VIDEO_YUV444_10BIT] = { .Bpp = 30 },
  [SOC_VIDEO_YUV444_12BIT] = { .Bpp = 36 },
  [SOC_VIDEO_YUV444_16BIT] = { .Bpp = 48 },

  [SOC_VIDEO_YUV422_8BIT]  = { .Bpp = 16 },
  [SOC_VIDEO_YUV422_10BIT] = { .Bpp = 20 },
  [SOC_VIDEO_YUV422_12BIT] = { .Bpp = 24 },
  [SOC_VIDEO_YUV422_16BIT] = { .Bpp = 32 },
};

STATIC
INTN
SocDpGetBpp (
  IN UINTN  Format
  )
{
  if (Format >= ARRAY_SIZE (FormatInfoTable)) {
    return 24;
  }

  return FormatInfoTable[Format].Bpp;
}

STATIC
UINTN
SocDpDiv64 (
  IN OUT UINT64  *N,
  IN     UINTN   Base
  )
{
  UINTN  Rem = (UINT32)(*N % Base);

  *N = *N / Base;
  return Rem;
}

STATIC
EFI_STATUS
SocDpRegWrite (
  IN SOC_DP_DEV  *Dp,
  IN UINTN       Offset,
  IN UINTN       BitWide,
  IN UINTN       Mask,
  IN UINTN       Val
  )
{
  UINTN  RegVal;

  RegVal  = MmioRead32 ((UINTN)Dp->Regs + Offset);
  RegVal &= ~Mask;
  RegVal |= Val & Mask;
  MmioWrite32 ((UINTN)Dp->Regs + Offset, RegVal);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SocDpRegWriteRange (
  IN SOC_DP_DEV  *Dp,
  IN UINTN       Offset,
  IN UINTN       High,
  IN UINTN       Low,
  IN UINTN       Val
  )
{
  UINTN  Mask;

  Mask = (UINT32)((((UINT64)1) << (High - Low + 1)) - 1) << Low;
  return SocDpRegWrite (Dp, Offset, 32, Mask, (Val << Low) & Mask);
}

STATIC
EFI_STATUS
SocDpRegOnlyWriteRange (
  IN SOC_DP_DEV  *Dp,
  IN UINTN       Offset,
  IN UINTN       High,
  IN UINTN       Low,
  IN UINTN       Val
  )
{
  UINTN  Mask;

  Mask = (UINT32)((((UINT64)1) << (High - Low + 1)) - 1) << Low;
  MmioWrite32 ((UINTN)Dp->Regs + Offset, (Val << Low) & Mask);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SocDpRegRead (
  IN  SOC_DP_DEV  *Dp,
  IN  UINTN       Offset,
  IN  UINTN       BitWide,
  IN  UINTN       Mask,
  OUT UINTN       *Val
  )
{
  *Val = MmioRead32 ((UINTN)Dp->Regs + Offset) & Mask;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SocDpRegReadRange (
  IN  SOC_DP_DEV  *Dp,
  IN  UINTN       Offset,
  IN  UINTN       High,
  IN  UINTN       Low,
  OUT UINTN       *Val
  )
{
  EFI_STATUS  Status;
  UINTN       Mask;

  Mask   = (UINT32)((((UINT64)1) << (High - Low + 1)) - 1) << Low;
  Status = SocDpRegRead (Dp, Offset, 32, Mask, Val);
  *Val   = *Val >> Low;

  return Status;
}

STATIC
VOID
SocDpAuxHwReset (
  IN SOC_DP_DEV  *Dp
  )
{
  SocDpRegWriteRange (Dp, SOC_DPTX_AUX_RESET, 0x1);
  MicroSecondDelay (2000);
  SocDpRegWriteRange (Dp, SOC_DPTX_AUX_RESET, 0x0);
  MicroSecondDelay (2000);
  SocDpRegOnlyWriteRange (Dp, SOC_DPTX_AUX_REPLY_EVENT_INT_STA, 1);
}

STATIC
INTN
SocDpAuxTransferRaw (
  IN  SOC_DP_DEV  *Dp,
  IN  UINTN       Request,
  IN  UINTN       Address,
  OUT UINT8       *Buf,
  IN  INTN        Size
  )
{
  INTN     Ret;
  INTN     i;
  UINTN    TimeoutCnt = 0;
  UINTN    Cmd, Len, Val, Status;
  UINTN    Data[4] = { 0 };
  BOOLEAN  IsRead;

  IsRead = (Request & DP_AUX_I2C_READ) ||
           ((Request & DP_AUX_NATIVE_READ) == DP_AUX_NATIVE_READ);

  /* 1. Check message validity */
  if (!Dp || !Buf || (Size <= 0) || (Size > 16) || (Address > 0xfffff) ||
      ((Request != DP_AUX_I2C_WRITE) && (Request != DP_AUX_I2C_READ) &&
       (Request != (DP_AUX_I2C_WRITE | DP_AUX_I2C_MOT)) &&
       (Request != (DP_AUX_I2C_READ | DP_AUX_I2C_MOT)) &&
       (Request != DP_AUX_NATIVE_WRITE) && (Request != DP_AUX_NATIVE_READ)))
  {
    return -EINVAL;
  }

  if (((Request & 8) && (Address + Size > 0x100000)) ||
      (!(Request & 8) && (Address > 0x7f)))
  {
    return -EINVAL;
  }

  Cmd = Request;

  /* 2. Prepare Data for Write (if applicable) */
  if (!IsRead) {
    for (i = 0; i < Size; i++) {
      Data[i / 4] |= (UINT32)Buf[i] << ((i % 4) * 8);
    }

    SocDpRegWriteRange (Dp, SOC_DPTX_AUX_DATA1, Data[0]);
    SocDpRegWriteRange (Dp, SOC_DPTX_AUX_DATA2, Data[1]);
    SocDpRegWriteRange (Dp, SOC_DPTX_AUX_DATA3, Data[2]);
    SocDpRegWriteRange (Dp, SOC_DPTX_AUX_DATA4, Data[3]);
  }

  /* 3. Configure Command, Address, Length */
  Len = Size > 0 ? Size - 1 : 0;

  SocDpRegWriteRange (Dp, SOC_DPTX_AUX_LENGTH, Len);
  SocDpRegWriteRange (Dp, SOC_DPTX_AUX_ADDR, Address);
  SocDpRegWriteRange (Dp, SOC_DPTX_AUX_CMD_TYPE, Cmd);

  /* 4. Trigger Transfer */
  SocDpRegOnlyWriteRange (Dp, SOC_DPTX_AUX_REPLY_EVENT_INT_STA, 1);
  SocDpRegWriteRange (Dp, SOC_DPTX_AUX_START, 0);
  SocDpRegWriteRange (Dp, SOC_DPTX_AUX_START, 1);

  /* 5. Wait for Completion */
  Ret = -ETIMEDOUT;
  while (TRUE) {
    SocDpRegReadRange (Dp, SOC_DPTX_AUX_REPLY_EVENT_INT_STA, &Val);
    if (Val != 0) {
      Ret = EFI_SUCCESS;
      break;
    }

    MicroSecondDelay (100);
    TimeoutCnt++;
    if (TimeoutCnt > 2000) {
      /* Approx 200ms */
      break;
    }
  }

  if (Ret < 0) {
    DP_DEBUG (Dp, (DEBUG_VERBOSE, "AUX transfer timeout\n"));
    return Ret;
  }

  /* 6. Read Status */
  SocDpRegReadRange (Dp, SOC_DPTX_AUX_STATUS, &Status);

  /* 7. Clear Interrupt Status (W1C) */
  SocDpRegOnlyWriteRange (Dp, SOC_DPTX_AUX_REPLY_EVENT_INT_STA, 1);

  /* K3 retains diagnostic error-code bits on valid ACKs. A verified
     receiver read reports 0x0004b049 with valid capability data, so those
     bits cannot independently reject the acknowledged payload. */
  /* The controller exposes the received AUX wire command byte.
     Reply command bits are 7:4; 0x20 is DEFER, not an unknown code.
     Reject nonzero reserved low bits before accepting any payload. */
  if (Status & 0xfU) {
    return -EIO;
  }

  Status >>= 4;

  /* The decoded low two reply bits are native AUX, the next two are I2C. */
  if ((Status & 3) == 1) {
    return -EIO;
  }

  if ((Status & 3) == 2) {
    return -EBUSY;
  }

  if ((Status & 3) == 3) {
    return -EIO;
  }

  if (!(Request & 8)) {
    Status >>= 2;
  }

  switch (Status) {
    case 0: /* ACK */
      break;
    case 1: /* NACK */
      DP_DEBUG (Dp, (DEBUG_VERBOSE, "AUX NACK: addr 0x%x\n", Address));
      return -EIO;
    case 2: /* DEFER */
      DP_DEBUG (Dp, (DEBUG_VERBOSE, "AUX DEFER: addr 0x%x\n", Address));
      return -EBUSY;
    default:
      SocDpRegReadRange (Dp, SOC_DPTX_AUX_REPLY_ERR_CODE, &Val);
      DP_DEBUG (
        Dp,
        (
         DEBUG_VERBOSE,
         "AUX info, cmd: 0x%x, address: 0x%x, size: %d, status: 0x%x, code: 0x%x\n",
         Cmd,
         Address,
         Size,
         Status,
         Val
        )
        );
      return -EIO;
  }

  /* 8. Read Data (if Read operation and ACK) */
  if (IsRead && (Size > 0)) {
    SocDpRegReadRange (Dp, SOC_DPTX_AUX_DATA1, &Data[0]);
    SocDpRegReadRange (Dp, SOC_DPTX_AUX_DATA2, &Data[1]);
    SocDpRegReadRange (Dp, SOC_DPTX_AUX_DATA3, &Data[2]);
    SocDpRegReadRange (Dp, SOC_DPTX_AUX_DATA4, &Data[3]);

    for (i = 0; i < Size; i++) {
      Buf[i] = (Data[i / 4] >> ((i % 4) * 8)) & 0xFF;
    }
  }

  return Size;
}

STATIC
INTN
SocDpAuxTransferWithRetry (
  IN SOC_DP_DEV  *Dp,
  IN UINTN       Cmd,
  IN UINTN       Address,
  IN OUT UINT8   *Data,
  IN INTN        Size
  )
{
  INTN        Retries = 0;
  INTN        Defers  = 0;
  INTN        Ret;
  CONST INTN  MaxRetries = 8;

  /* Match Linux native AUX tolerance for slow receivers, while keeping
     the existing timeout/error budget independent and bounded. */
  while (Retries < MaxRetries && Defers < 32) {
    Ret = SocDpAuxTransferRaw (Dp, Cmd, Address, Data, Size);

    if (Ret >= 0) {
      return Ret;
    }

    /* If Timeout, reset aux and retry */
    if (Ret == -ETIMEDOUT) {
      SocDpAuxHwReset (Dp);
      Retries++;
      continue;
    }

    /* If DEFER (Sink busy), wait and retry */
    if (Ret == -EBUSY) {
      MicroSecondDelay (500);
      Defers++;
      continue;
    }

    /* If NACK, retry briefly just in case */
    if (Ret == -EIO) {
      MicroSecondDelay (100);
      Retries++;
      continue;
    }

    return Ret;
  }

  DP_DEBUG (
    Dp,
    (
     DEBUG_ERROR,
     "AUX transfer exhausted errors=%d defers=%d (cmd 0x%x, addr 0x%x)\n",
     Retries,
     Defers,
     Cmd,
     Address
    )
    );
  return Ret;
}

STATIC
INTN
SocDpAuxNativeWrite (
  IN SOC_DP_DEV  *Dp,
  IN UINTN       Address,
  IN UINT8       *Data,
  IN INTN        Size
  )
{
  return SocDpAuxTransferWithRetry (Dp, DP_AUX_NATIVE_WRITE, Address, Data, Size);
}

STATIC
INTN
SocDpAuxNativeRead (
  IN SOC_DP_DEV  *Dp,
  IN UINTN       Address,
  OUT UINT8      *Data,
  IN INTN        Size
  )
{
  return SocDpAuxTransferWithRetry (Dp, DP_AUX_NATIVE_READ, Address, Data, Size);
}

STATIC
INTN
SocDpDpcdWrite (
  IN SOC_DP_DEV  *Dp,
  IN UINTN       Address,
  IN UINT8       *Buf,
  IN INTN        Size
  )
{
  return SocDpAuxNativeWrite (Dp, Address, Buf, Size);
}

STATIC
INTN
SocDpDpcdWriteb (
  IN SOC_DP_DEV  *Dp,
  IN UINTN       Address,
  IN UINT8       Data
  )
{
  return SocDpDpcdWrite (Dp, Address, &Data, 1);
}

STATIC
INTN
SocDpDpcdRead (
  IN  SOC_DP_DEV  *Dp,
  IN  UINTN       Address,
  OUT UINT8       *Buf,
  IN  INTN        Size
  )
{
  return SocDpAuxNativeRead (Dp, Address, Buf, Size);
}

STATIC
INTN
SocDpAuxI2cWrite (
  IN SOC_DP_DEV  *Dp,
  IN UINTN       Address,
  IN UINT8       *Data,
  IN INTN        Size
  )
{
  return SocDpAuxTransferWithRetry (Dp, DP_AUX_I2C_WRITE, Address, Data, Size);
}

STATIC
BOOLEAN
SocDpDpcdCapsValid (
  IN CONST UINT8  *Dpcd
  )
{
  UINT8  MaxBw    = Dpcd[DP_MAX_LINK_RATE];
  UINT8  MaxLanes = Dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK;

  if (Dpcd[DP_DPCD_REV] == 0) {
    return FALSE;
  }

  switch (MaxBw) {
    case DP_LINK_BW_1_62:
    case DP_LINK_BW_2_7:
    case DP_LINK_BW_5_4:
    case DP_LINK_BW_8_1:
      break;
    default:
      return FALSE;
  }

  return (MaxLanes == 1 || MaxLanes == 2 || MaxLanes == 4);
}

INTN
SocDpHwReadSinkCaps (
  IN SOC_DP_DEV  *Dp
  )
{
  INTN   Ret;
  UINT8  MaxBw;
  INTN   Retry;

  if (!Dp || !Dp->Ready) {
    return -EINVAL;
  }

  ZeroMem (&Dp->Link, sizeof (Dp->Link));

  for (Retry = 0; Retry < 3; Retry++) {
    if (Retry != 0) {
      SocDpAuxHwReset (Dp);
      MicroSecondDelay (10000);
    }

    /* 1. Read DPCD Receiver Capability fields (0x00000 - 0x0000F) */
    Ret = SocDpDpcdRead (Dp, DP_DPCD_REV, Dp->Dpcd, DP_RECEIVER_CAP_SIZE);
    if (Ret < 0) {
      continue;
    }

    if ((Ret != DP_RECEIVER_CAP_SIZE) || !SocDpDpcdCapsValid (Dp->Dpcd)) {
      DP_DEBUG (
        Dp,
        (
         DEBUG_VERBOSE,
         "DPCD caps not ready: rev=0x%02x bw=0x%02x lanes=0x%02x\n",
         Dp->Dpcd[DP_DPCD_REV],
         Dp->Dpcd[DP_MAX_LINK_RATE],
         Dp->Dpcd[DP_MAX_LANE_COUNT]
        )
        );
      Ret = -EAGAIN;
      continue;
    }

    break;
  }

  if (Ret < 0) {
    DP_DEBUG (Dp, (DEBUG_ERROR, "Failed to read DPCD\n"));
    return Ret;
  }

  /* 2. Parse DP Revision */
  Dp->Link.Revision = Dp->Dpcd[DP_DPCD_REV];

  /* 3. Parse and determine Link Rate */
  MaxBw = Dp->Dpcd[DP_MAX_LINK_RATE];
  switch (MaxBw) {
    case DP_LINK_BW_1_62:
      Dp->Link.MaxRate = SOC_DP_LINK_RATE_1_62;
      break;
    case DP_LINK_BW_2_7:
      Dp->Link.MaxRate = SOC_DP_LINK_RATE_2_70;
      break;
    case DP_LINK_BW_5_4:
      Dp->Link.MaxRate = SOC_DP_LINK_RATE_5_40;
      break;
    case DP_LINK_BW_8_1:
      Dp->Link.MaxRate = SOC_DP_LINK_RATE_8_10;
      break;
    default:
      return -EINVAL;
  }

  /* 4. Parse and determine Lane Count */
  Dp->Link.MaxRate     = MIN (Dp->Link.MaxRate, SOC_DP_LINK_RATE_5_40);
  Dp->Link.MaxNumLanes = Dp->Dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK;

  /* 5. Check for Enhanced Framing support */
  Dp->Link.EnhancedFraming = (Dp->Dpcd[DP_MAX_LANE_COUNT] & DP_ENHANCED_FRAME_CAP) ? 1 : 0;

  DP_DEBUG (
    Dp,
    (
     DEBUG_INFO,
     "DPCD: Rev %x.%x, MaxRate %d kHz, MaxLanes %d, EnhFrame %d\n",
     Dp->Link.Revision >> 4,
     Dp->Link.Revision & 0xF,
     Dp->Link.MaxRate,
     Dp->Link.MaxNumLanes,
     Dp->Link.EnhancedFraming
    )
    );

  return 0;
}

SOC_DP_CONNECTOR_STATUS
SocDpHwDetectHpd (
  IN SOC_DP_DEV  *Dp
  )
{
  UINTN                    HpdStatus;
  SOC_DP_CONNECTOR_STATUS  ConnectorStatus;

  if (!Dp || !Dp->Ready) {
    return ConnectorStatusUnknown;
  }

  SocDpRegReadRange (Dp, SOC_DPTX_HPD_IN_STATUS, &HpdStatus);

  if (HpdStatus != 0) {
    ConnectorStatus = ConnectorStatusConnected;
  } else {
    ConnectorStatus = ConnectorStatusDisconnected;
    ZeroMem (&Dp->Link, sizeof (Dp->Link));
    Dp->EdidSegment = 0;
    Dp->Trained     = FALSE;
  }

  Dp->ConnectorStatus = ConnectorStatus;
  return ConnectorStatus;
}

VOID
SocDpHwCleanHpd (
  IN SOC_DP_DEV  *Dp
  )
{
  UINTN  PlugEvent, UnplugEvent;

  if (!Dp || !Dp->Ready) {
    return;
  }

  SocDpRegReadRange (Dp, SOC_DPTX_HOT_PLUG_EVENT, &PlugEvent);
  SocDpRegReadRange (Dp, SOC_DPTX_HOT_UNPLUG_EVENT, &UnplugEvent);

  if (PlugEvent != 0) {
    SocDpRegOnlyWriteRange (Dp, SOC_DPTX_HOT_PLUG_EVENT, 0x1);
  }

  if (UnplugEvent != 0) {
    SocDpRegOnlyWriteRange (Dp, SOC_DPTX_HOT_UNPLUG_EVENT, 0x1);
  }
}

STATIC
INTN
SocDpSetTrainingPattern (
  IN SOC_DP_DEV  *Dp,
  IN UINT8       Pattern
  )
{
  UINTN  TpsSel      = 0;
  UINT8  DpcdPattern = Pattern;
  INTN   Ret;

  if (Pattern != DP_TRAINING_PATTERN_DISABLE) {
    DpcdPattern |= DP_LINK_SCRAMBLING_DISABLE;
  }

  /* Configure PHY Pattern */
  switch (Pattern) {
    case DP_TRAINING_PATTERN_DISABLE:
      TpsSel = 0;
      SocDpRegWriteRange (Dp, SOC_DPTX_SCRAMBLER_DISABLE, 0);
      break;
    case DP_TRAINING_PATTERN_1:
      TpsSel = 1;
      SocDpRegWriteRange (Dp, SOC_DPTX_SCRAMBLER_DISABLE, 1);
      break;
    case DP_TRAINING_PATTERN_2:
      TpsSel = 2;
      SocDpRegWriteRange (Dp, SOC_DPTX_SCRAMBLER_DISABLE, 1);
      break;
    case DP_TRAINING_PATTERN_3:
      TpsSel = 3;
      SocDpRegWriteRange (Dp, SOC_DPTX_SCRAMBLER_DISABLE, 1);
      break;
    default:
      DP_DEBUG (Dp, (DEBUG_ERROR, "Unsupported training pattern: 0x%x\n", Pattern));
      return -EINVAL;
  }

  SocDpRegWriteRange (Dp, SOC_DPTX_TPS_SEL, TpsSel);

  /* Configure DPCD Pattern */
  Ret = SocDpDpcdWriteb (Dp, DP_TRAINING_PATTERN_SET, DpcdPattern);
  if (Ret < 0) {
    DP_DEBUG (Dp, (DEBUG_ERROR, "Failed to set DPCD training pattern: %d\n", Ret));
    return Ret;
  }

  return 0;
}

STATIC
INTN
SocDpLinkStatusCrOk (
  IN UINT8  *LinkStatus,
  IN INTN   LaneCount
  )
{
  INTN   Lane;
  UINT8  LaneStatus;

  for (Lane = 0; Lane < LaneCount; Lane++) {
    LaneStatus = LinkStatus[Lane >> 1];
    if ((Lane & 1) != 0) {
      LaneStatus >>= 4;
    }

    if ((LaneStatus & DP_LANE_CR_DONE) == 0) {
      return 0;
    }
  }

  return 1;
}

STATIC
INTN
SocDpLinkStatusEqOk (
  IN UINT8  *LinkStatus,
  IN INTN   LaneCount
  )
{
  INTN   Lane;
  UINT8  LaneStatus;
  UINT8  Align = LinkStatus[2];

  if ((Align & 1) == 0) {
    return 0;
  }

  for (Lane = 0; Lane < LaneCount; Lane++) {
    LaneStatus = LinkStatus[Lane >> 1];
    if ((Lane & 1) != 0) {
      LaneStatus >>= 4;
    }

    if ((LaneStatus & (DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED)) !=
        (DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED))
    {
      return 0;
    }
  }

  return 1;
}

STATIC
UINT8
SocDpGetAdjustReqV (
  IN UINT8  *LinkStatus,
  IN INTN   Lane
  )
{
  UINT8  Req = LinkStatus[DP_ADJUST_REQUEST_LANE0_1 - DP_LANE0_1_STATUS + (Lane >> 1)];

  if ((Lane & 1) != 0) {
    Req >>= 4;
  }

  return Req & DP_TRAIN_VOLTAGE_SWING_MASK;
}

STATIC
UINT8
SocDpGetAdjustReqP (
  IN UINT8  *LinkStatus,
  IN INTN   Lane
  )
{
  UINT8  Req = LinkStatus[DP_ADJUST_REQUEST_LANE0_1 - DP_LANE0_1_STATUS + (Lane >> 1)];

  if ((Lane & 1) != 0) {
    Req >>= 4;
  }

  return (Req >> 2) & 3;
}

typedef enum {
  SOC_DP_TRAIN_DELAY_CLOCK_RECOVERY,
  SOC_DP_TRAIN_DELAY_CHANNEL_EQ,
} SOC_DP_LINK_TRAIN_DELAY;

STATIC
VOID
SocDpLinkTrainDelay (
  IN SOC_DP_DEV               *Dp,
  IN SOC_DP_LINK_TRAIN_DELAY  DelayType
  )
{
  UINT8   Interval = Dp->Dpcd[DP_TRAINING_AUX_RD_INTERVAL] & DP_TRAINING_AUX_RD_MASK;
  UINT16  DelayUs  = (DelayType == SOC_DP_TRAIN_DELAY_CLOCK_RECOVERY) ? 100 : 400;

  if ((DelayType == SOC_DP_TRAIN_DELAY_CLOCK_RECOVERY) && (Dp->Dpcd[0] >= 0x14)) {
    MicroSecondDelay (100);
    return;
  }

  if (Interval == 0) {
    MicroSecondDelay (DelayUs);
    return;
  }

  if (Interval <= 4) {
    MicroSecondDelay (Interval * 4000);
    return;
  }

  DP_DEBUG (
    Dp,
    (
     DEBUG_WARN,
     "Invalid TRAINING_AUX_RD_INTERVAL 0x%x, fallback to %uus\n",
     Interval,
     DelayUs
    )
    );
  MicroSecondDelay (DelayUs);
}

STATIC
INTN
SocDpLinkTrainClockRecovery (
  IN SOC_DP_DEV         *Dp,
  IN SOC_DP_LINK_RATE   Rate,
  IN SOC_DP_LANE_COUNT  Lanes
  )
{
  UINT8                      LinkStatus[DP_LINK_STATUS_SIZE];
  UINT8                      TrainingSet[4] = { 0 };
  INTN                       Retries = 0;
  INTN                       i, Ret;
  SOC_DP_PHY_CONFIGURE_OPTS  PhyOpts;

  ZeroMem (&PhyOpts, sizeof (PhyOpts));
  PhyOpts.Lanes       = Lanes;
  PhyOpts.SetVoltages = 1;

  Ret = SocDpPhyConfigure (&Dp->Phy, &PhyOpts);
  if (Ret) {
    return Ret;
  }

  Ret = SocDpDpcdWrite (Dp, DP_TRAINING_LANE0_SET, TrainingSet, Lanes);
  if (Ret < 0) {
    return Ret;
  }

  Ret = SocDpSetTrainingPattern (Dp, DP_TRAINING_PATTERN_1);
  if (Ret < 0) {
    SocDpSetTrainingPattern (Dp, DP_TRAINING_PATTERN_DISABLE);
    return Ret;
  }

  while (Retries < 8) {
    SocDpLinkTrainDelay (Dp, SOC_DP_TRAIN_DELAY_CLOCK_RECOVERY);

    Ret = SocDpDpcdRead (Dp, DP_LANE0_1_STATUS, LinkStatus, DP_LINK_STATUS_SIZE);
    if (Ret < 0) {
      SocDpSetTrainingPattern (Dp, DP_TRAINING_PATTERN_DISABLE);
      return Ret;
    }

    if (SocDpLinkStatusCrOk (LinkStatus, Lanes) != 0) {
      return 0;
    }

    /* Update settings based on Sink request */
    for (i = 0; i < Lanes; i++) {
      UINT8  V = SocDpGetAdjustReqV (LinkStatus, i);
      UINT8  P = SocDpGetAdjustReqP (LinkStatus, i);

      V              = MIN (V, SOC_DP_SWING_MAX);
      P              = MIN (P, MIN (SOC_DP_PREEMP_MAX, 3 - V));
      TrainingSet[i] = V | (P << DP_TRAIN_PRE_EMPHASIS_SHIFT);
      /* Pre-emphasis also limits the maximum available voltage swing. */
      if (V == MIN (SOC_DP_SWING_MAX, 3 - P)) {
        TrainingSet[i] |= DP_TRAIN_MAX_SWING_REACHED;
      }

      if (P == MIN (SOC_DP_PREEMP_MAX, 3 - V)) {
        TrainingSet[i] |= DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;
      }

      PhyOpts.Voltage[i] = V;
      PhyOpts.Pre[i]     = P;
    }

    Ret = SocDpPhyConfigure (&Dp->Phy, &PhyOpts);
    if (Ret != 0) {
      return Ret;
    }

    Ret = SocDpDpcdWrite (Dp, DP_TRAINING_LANE0_SET, TrainingSet, Lanes);
    if (Ret < 0) {
      SocDpSetTrainingPattern (Dp, DP_TRAINING_PATTERN_DISABLE);
      return Ret;
    }

    Retries++;
  }

  DP_DEBUG (Dp, (DEBUG_ERROR, "Link Training Clock Recovery Failed\n"));
  DP_DEBUG (
    Dp,
    (
     DEBUG_ERROR,
     "CR last sink status: %02x %02x %02x %02x %02x %02x\n",
     LinkStatus[0], LinkStatus[1], LinkStatus[2],
     LinkStatus[3], LinkStatus[4], LinkStatus[5]
    )
    );
  SocDpSetTrainingPattern (Dp, DP_TRAINING_PATTERN_DISABLE);
  return -ETIMEDOUT;
}

STATIC
INTN
SocDpLinkTrainChannelEq (
  IN SOC_DP_DEV         *Dp,
  IN SOC_DP_LINK_RATE   Rate,
  IN SOC_DP_LANE_COUNT  Lanes
  )
{
  UINT8                      LinkStatus[DP_LINK_STATUS_SIZE];
  UINT8                      TrainingSet[4] = { 0 };
  UINT8                      TrainingPattern = DP_TRAINING_PATTERN_2;
  INTN                       Retries = 0;
  INTN                       i, Ret;
  SOC_DP_PHY_CONFIGURE_OPTS  PhyOpts;

  ZeroMem (&PhyOpts, sizeof (PhyOpts));
  PhyOpts.Lanes       = Lanes;
  PhyOpts.SetVoltages = 1;

  if ((Dp->Dpcd[DP_MAX_LANE_COUNT] & DP_TPS3_SUPPORTED) != 0) {
    TrainingPattern = DP_TRAINING_PATTERN_3;
    DP_DEBUG (Dp, (DEBUG_INFO, "Link Training: Using TPS3\n"));
  } else {
    DP_DEBUG (Dp, (DEBUG_INFO, "Link Training: Using TPS2\n"));
  }

  Ret = SocDpSetTrainingPattern (Dp, TrainingPattern);
  if (Ret < 0) {
    SocDpSetTrainingPattern (Dp, DP_TRAINING_PATTERN_DISABLE);
    return Ret;
  }

  while (Retries < 8) {
    SocDpLinkTrainDelay (Dp, SOC_DP_TRAIN_DELAY_CHANNEL_EQ);

    Ret = SocDpDpcdRead (Dp, DP_LANE0_1_STATUS, LinkStatus, DP_LINK_STATUS_SIZE);
    if (Ret < 0) {
      SocDpSetTrainingPattern (Dp, DP_TRAINING_PATTERN_DISABLE);
      return Ret;
    }

    if (SocDpLinkStatusCrOk (LinkStatus, Lanes) && SocDpLinkStatusEqOk (LinkStatus, Lanes)) {
      return SocDpSetTrainingPattern (Dp, DP_TRAINING_PATTERN_DISABLE);
    }

    /* Update settings based on Sink request */
    for (i = 0; i < Lanes; i++) {
      UINT8  V = SocDpGetAdjustReqV (LinkStatus, i);
      UINT8  P = SocDpGetAdjustReqP (LinkStatus, i);

      V              = MIN (V, SOC_DP_SWING_MAX);
      P              = MIN (P, MIN (SOC_DP_PREEMP_MAX, 3 - V));
      TrainingSet[i] = V | (P << DP_TRAIN_PRE_EMPHASIS_SHIFT);
      /* Pre-emphasis also limits the maximum available voltage swing. */
      if (V == MIN (SOC_DP_SWING_MAX, 3 - P)) {
        TrainingSet[i] |= DP_TRAIN_MAX_SWING_REACHED;
      }

      if (P == MIN (SOC_DP_PREEMP_MAX, 3 - V)) {
        TrainingSet[i] |= DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;
      }

      PhyOpts.Voltage[i] = V;
      PhyOpts.Pre[i]     = P;
    }

    Ret = SocDpPhyConfigure (&Dp->Phy, &PhyOpts);
    if (Ret != 0) {
      return Ret;
    }

    Ret = SocDpDpcdWrite (Dp, DP_TRAINING_LANE0_SET, TrainingSet, Lanes);
    if (Ret < 0) {
      SocDpSetTrainingPattern (Dp, DP_TRAINING_PATTERN_DISABLE);
      return Ret;
    }

    Retries++;
  }

  DP_DEBUG (Dp, (DEBUG_ERROR, "Link Training Channel EQ Failed\n"));
  DP_DEBUG (
    Dp,
    (
     DEBUG_ERROR,
     "EQ last sink status: %02x %02x %02x %02x %02x %02x\n",
     LinkStatus[0], LinkStatus[1], LinkStatus[2],
     LinkStatus[3], LinkStatus[4], LinkStatus[5]
    )
    );
  SocDpSetTrainingPattern (Dp, DP_TRAINING_PATTERN_DISABLE);
  return -ETIMEDOUT;
}

STATIC
INTN
SocDpLinkTrain (
  IN SOC_DP_DEV         *Dp,
  IN SOC_DP_LINK_RATE   Rate,
  IN SOC_DP_LANE_COUNT  Lanes
  )
{
  INTN   Ret;
  UINT8  LaneConfig;
  UINT8  BwCode;

  /* Map Link Rate Enum to DPCD Bandwidth Code */
  switch (Rate) {
    case SOC_DP_LINK_RATE_1_62:
      BwCode = DP_LINK_BW_1_62;
      break;
    case SOC_DP_LINK_RATE_2_70:
      BwCode = DP_LINK_BW_2_7;
      break;
    case SOC_DP_LINK_RATE_5_40:
      BwCode = DP_LINK_BW_5_4;
      break;
    case SOC_DP_LINK_RATE_8_10:
      BwCode = DP_LINK_BW_8_1;
      break;
    default:
      BwCode = DP_LINK_BW_1_62;
      break;
  }

  /* A sink can latch its configuration when bandwidth is written.
     Establish lane count in a separate transaction before that trigger,
     matching the working K3 Linux controller path. */
  LaneConfig = (UINT8)Lanes;
  if (Dp->Link.EnhancedFraming != 0) {
    LaneConfig |= DP_LANE_COUNT_ENHANCED_FRAME_EN;
  }

  Ret = SocDpDpcdWriteb (Dp, DP_LANE_COUNT_SET, LaneConfig);
  if (Ret < 0) {
    DP_DEBUG (Dp, (DEBUG_ERROR, "Failed to configure DPCD lane count\n"));
    return Ret;
  }

  Ret = SocDpDpcdWriteb (Dp, DP_LINK_BW_SET, BwCode);
  if (Ret < 0) {
    DP_DEBUG (Dp, (DEBUG_ERROR, "Failed to configure DPCD link bandwidth\n"));
    return Ret;
  }

  Ret = SocDpLinkTrainClockRecovery (Dp, Rate, Lanes);
  if (Ret != 0) {
    return Ret;
  }

  Ret = SocDpLinkTrainChannelEq (Dp, Rate, Lanes);
  if (Ret != 0) {
    return Ret;
  }

  MicroSecondDelay (20000);
  return 0;
}

STATIC
VOID
SocDpHwSetMsa (
  IN SOC_DP_DEV               *Dp,
  IN CONST SOC_DP_VIDEO_MODE  *Mode,
  IN SOC_DP_LINK_RATE         Rate,
  IN SOC_DP_LANE_COUNT        Lanes
  )
{
  UINT64  HbNum;
  UINTN   LinkRateKhz;
  UINTN   PixelClkKhz;
  UINTN   Bpp, Misc0;
  UINTN   Tu, TuFrac, TuInt, RdThres;
  UINTN   HsyncLen;

  /* 1. Prepare basic parameters */
  PixelClkKhz = Mode->Clock;
  if (PixelClkKhz == 0) {
    PixelClkKhz = 1;
  }

  Bpp = SocDpGetBpp (Dp->ColorFormat);

  /* Calculate MISC0 */
  switch (Dp->ColorFormat) {
    case SOC_VIDEO_RGB_6BIT:      Misc0 = 0x00;
      break;
    case SOC_VIDEO_RGB_8BIT:      Misc0 = 0x20;
      break;
    case SOC_VIDEO_RGB_10BIT:     Misc0 = 0x40;
      break;
    case SOC_VIDEO_RGB_12BIT:     Misc0 = 0x60;
      break;
    case SOC_VIDEO_RGB_16BIT:     Misc0 = 0x80;
      break;
    case SOC_VIDEO_YUV422_8BIT:   Misc0 = 0x22;
      break;
    case SOC_VIDEO_YUV422_10BIT:  Misc0 = 0x42;
      break;
    case SOC_VIDEO_YUV422_12BIT:  Misc0 = 0x62;
      break;
    case SOC_VIDEO_YUV422_16BIT:  Misc0 = 0x82;
      break;
    case SOC_VIDEO_YUV444_8BIT:   Misc0 = 0x24;
      break;
    case SOC_VIDEO_YUV444_10BIT:  Misc0 = 0x44;
      break;
    case SOC_VIDEO_YUV444_12BIT:  Misc0 = 0x64;
      break;
    case SOC_VIDEO_YUV444_16BIT:  Misc0 = 0x84;
      break;
    default:                      Misc0 = 0x20;
      break;
  }

  /* 2. Calculate HBlank Interval (hb_num) */
  LinkRateKhz = Rate;

  HbNum = (UINT64)(Mode->Htotal - Mode->Hdisplay) * LinkRateKhz;
  {
    UINTN  Den = 40 * PixelClkKhz;

    HbNum += (Den / 2);
    SocDpDiv64 (&HbNum, Den);
  }

  /* 3. Calculate TU (Transfer Unit) */
  {
    UINT64  TempTu = (UINT64)PixelClkKhz * Bpp * 800;
    UINTN   Den    = (UINT32)Lanes * LinkRateKhz;

    TempTu += (Den / 2);
    SocDpDiv64 (&TempTu, Den);
    Tu = (UINT32)TempTu;
  }
  TuFrac = Tu % 10;
  TuInt  = Tu / 10;

  /* 4. Calculate FIFO read threshold */
  if (TuInt < 6) {
    RdThres = 32;
  } else if ((Mode->Htotal - Mode->Hdisplay) < 80) {
    RdThres = 12;
  } else {
    RdThres = 16;
  }

  DP_DEBUG (
    Dp,
    (
     DEBUG_INFO,
     "MSA: %dx%d, Rate:%d kHz, Lanes:%d, BPP:%d, TU:%d.%d\n",
     Mode->Hdisplay,
     Mode->Vdisplay,
     Rate,
     Lanes,
     Bpp,
     TuInt,
     TuFrac
    )
    );

  /* 5. Video mapping format */
  SocDpRegWriteRange (Dp, SOC_DPTX_VIDEO_MAPPING, Dp->ColorFormat);

  /* Polarity configuration */
  if ((Mode->Flags & SOC_DP_MODE_FLAG_PHSYNC) != 0) {
    SocDpRegWriteRange (Dp, SOC_DPTX_HSYNC_IN_POLARITY, 1);
  } else {
    SocDpRegWriteRange (Dp, SOC_DPTX_HSYNC_IN_POLARITY, 0);
  }

  if ((Mode->Flags & SOC_DP_MODE_FLAG_PVSYNC) != 0) {
    SocDpRegWriteRange (Dp, SOC_DPTX_VSYNC_IN_POLARITY, 1);
  } else {
    SocDpRegWriteRange (Dp, SOC_DPTX_VSYNC_IN_POLARITY, 0);
  }

  /* Basic timing */
  SocDpRegWriteRange (Dp, SOC_DPTX_HACTIVE, Mode->Hdisplay);
  SocDpRegWriteRange (Dp, SOC_DPTX_VACTIVE, Mode->Vdisplay);
  SocDpRegWriteRange (Dp, SOC_DPTX_HBLANK, Mode->Htotal - Mode->Hdisplay);
  SocDpRegWriteRange (Dp, SOC_DPTX_VBLANK, Mode->Vtotal - Mode->Vdisplay);

  SocDpRegWriteRange (Dp, SOC_DPTX_HSTART, Mode->Htotal - Mode->HsyncStart);
  SocDpRegWriteRange (Dp, SOC_DPTX_VSTART, Mode->Vtotal - Mode->VsyncStart);

  HsyncLen = Mode->HsyncEnd - Mode->HsyncStart;

  SocDpRegWriteRange (Dp, SOC_DPTX_H_SYNC_WIDTH, HsyncLen);
  SocDpRegWriteRange (Dp, SOC_DPTX_V_SYNC_WIDTH, Mode->VsyncEnd - Mode->VsyncStart);
  SocDpRegWriteRange (Dp, SOC_DPTX_H_FRONT_PORCH, Mode->HsyncStart - Mode->Hdisplay);
  SocDpRegWriteRange (Dp, SOC_DPTX_V_FRONT_PORCH, Mode->VsyncStart - Mode->Vdisplay);

  /* MSA and MISC */
  SocDpRegWriteRange (Dp, SOC_DPTX_MISC0, Misc0);
  SocDpRegWriteRange (Dp, SOC_DPTX_MISC1, 0);
  SocDpRegWriteRange (Dp, SOC_DPTX_NVID, 0);

  /* Link layer parameters */
  SocDpRegWriteRange (Dp, SOC_DPTX_HBLANK_INTERVAL, (UINT32)HbNum);
  SocDpRegWriteRange (Dp, SOC_DPTX_AVERAGE_BYTES_PER_TU, TuInt);
  SocDpRegWriteRange (Dp, SOC_DPTX_AVERAGE_BYTES_PER_TU_FRAC, TuFrac);
  SocDpRegWriteRange (Dp, SOC_DPTX_INIT_THRESHOLD, RdThres);

  SocDpRegWriteRange (Dp, SOC_DPTX_REG_VID_CLK_SEL, 0);
  SocDpRegWriteRange (Dp, SOC_DPTX_VID_BIST_EN, 0);
}

INTN
SocDpHwCheckLink (
  IN SOC_DP_DEV  *Dp,
  OUT UINT8      *Status OPTIONAL
  )
{
  UINTN  Locked;
  UINT8  LinkStatus[DP_LINK_STATUS_SIZE];
  INTN   Ret;

  if (Status) {
    ZeroMem (Status, DP_LINK_STATUS_SIZE);
  }

  if (!Dp || !Dp->Ready || !Dp->Trained || Dp->CleanupError) {
    return -EINVAL;
  }

  if (!Dp->EdpMode && (SocDpHwDetectHpd (Dp) != ConnectorStatusConnected)) {
    DP_DEBUG (Dp, (DEBUG_WARN, "DP stream check: HPD is low\n"));
    return -ENOLINK;
  }

  Ret = -EIO;
  SocDpRegReadRange (Dp, SOC_DPTX_AD_LOCK_COREPLL, &Locked);
  if (!Locked) {
    DP_DEBUG (Dp, (DEBUG_WARN, "DP stream check: core PLL is unlocked\n"));
    return -EIO;
  }

  SocDpRegReadRange (Dp, SOC_DPTX_AD_LOCK_PIXELPLL, &Locked);
  if (!Locked) {
    DP_DEBUG (Dp, (DEBUG_WARN, "DP stream check: pixel PLL is unlocked\n"));
    return -EIO;
  }

  Ret = SocDpDpcdRead (Dp, DP_LANE0_1_STATUS, LinkStatus, sizeof (LinkStatus));
  if (Ret < 0) {
    DP_DEBUG (Dp, (DEBUG_WARN, "DP stream check: link-status read failed %d\n", Ret));
    return Ret;
  }

  if (Status) {
    CopyMem (Status, LinkStatus, sizeof (LinkStatus));
  }

  if (!SocDpLinkStatusCrOk (LinkStatus, Dp->Phy.LaneCount) ||
      !SocDpLinkStatusEqOk (LinkStatus, Dp->Phy.LaneCount))
  {
    DP_DEBUG (
      Dp,
      (
       DEBUG_WARN,
       "DP stream check: receiver status %02x %02x %02x %02x %02x %02x\n",
       LinkStatus[0],
       LinkStatus[1],
       LinkStatus[2],
       LinkStatus[3],
       LinkStatus[4],
       LinkStatus[5]
      )
      );
    return -ENOLINK;
  }

  return 0;
}

INTN
SocDpHwEnable (
  IN SOC_DP_DEV  *Dp
  )
{
  UINTN  Locked;
  INTN   Ret = SocDpHwCheckLink (Dp, NULL);

  if (Ret == -EINVAL) {
    return Ret;
  }

  if (Ret) {
    goto Fail;
  }

  DP_DEBUG (Dp, (DEBUG_VERBOSE, "Enabling Video Stream\n"));
  SocDpRegWriteRange (Dp, SOC_DPTX_VIDEO_STREAM_ENABLE, 1);
  SocDpRegReadRange (Dp, SOC_DPTX_VIDEO_STREAM_ENABLE, &Locked);
  Dp->Streaming = Locked != 0;
  if (Dp->Streaming) {
    return 0;
  }

  Ret = -EIO;
Fail:
  Dp->CleanupError = SocDpHwDisable (Dp);
  if (!Dp->CleanupError) {
    Dp->CleanupError = SocDpPhyPowerOff (&Dp->Phy);
  }

  return Ret;
}

INTN
SocDpHwDisable (
  IN SOC_DP_DEV  *Dp
  )
{
  UINTN  Enabled;

  if (!Dp || !Dp->Regs) {
    return -EINVAL;
  }

  DP_DEBUG (Dp, (DEBUG_VERBOSE, "Disabling Video Stream\n"));
  SocDpRegWriteRange (Dp, SOC_DPTX_VIDEO_STREAM_ENABLE, 0);
  SocDpRegReadRange (Dp, SOC_DPTX_VIDEO_STREAM_ENABLE, &Enabled);
  Dp->Streaming = Enabled != 0;
  Dp->Trained   = FALSE;
  if (Enabled) {
    Dp->CleanupError = -EIO;
    return -EIO;
  }

  return 0;
}

INTN
SocDpExit (
  IN SOC_DP_DEV  *Dp
  )
{
  if (!Dp || !Dp->Regs) {
    return -EINVAL;
  }

  INTN  Ret = SocDpHwDisable (Dp);

  if (!Ret) {
    Ret = SocDpPhyExit (&Dp->Phy);
  }

  Dp->CleanupError = Ret;
  if (!Ret) {
    Dp->Ready = FALSE;
  }

  return Ret;
}

BOOLEAN
SocDpModeValid (
  IN CONST SOC_DP_VIDEO_MODE  *Mode
  )
{
  return Mode && Mode->Clock && Mode->Clock <= 1500000 && Mode->Hdisplay && Mode->Vdisplay &&
         Mode->Hdisplay <= Mode->HsyncStart && Mode->HsyncStart < Mode->HsyncEnd &&
         Mode->HsyncEnd <= Mode->Htotal && Mode->Vdisplay <= Mode->VsyncStart &&
         Mode->VsyncStart < Mode->VsyncEnd && Mode->VsyncEnd <= Mode->Vtotal &&
         !(Mode->Flags & ~(SOC_DP_MODE_FLAG_PHSYNC | SOC_DP_MODE_FLAG_PVSYNC));
}

EFI_STATUS
SocDpStatusToEfi (
  IN INTN  Status
  )
{
  if (Status >= 0) {
    return EFI_SUCCESS;
  }

  switch (Status) {
    case -EINVAL: return EFI_INVALID_PARAMETER;
    case -ETIMEDOUT: return EFI_TIMEOUT;
    case -EBUSY: case -EAGAIN: return EFI_NOT_READY;
    case -ENOLINK: return EFI_NO_MEDIA;
    case -EOPNOTSUPP: return EFI_UNSUPPORTED;
    default: return EFI_DEVICE_ERROR;
  }
}

STATIC
UINTN
SocDpCalcRequiredBw (
  IN CONST SOC_DP_VIDEO_MODE  *Mode,
  IN INTN                     Bpp
  )
{
  return Mode->Clock * Bpp;
}

STATIC
UINTN
SocDpCalcLinkCapacity (
  IN SOC_DP_LINK_RATE   Rate,
  IN SOC_DP_LANE_COUNT  Lanes
  )
{
  return (Rate * Lanes * 8) / 10;
}

INTN
SocDpSinkPower (
  IN SOC_DP_DEV  *Dp,
  IN BOOLEAN     On
  )
{
  UINT8  Power;
  INTN   Ret;

  if (!Dp || !Dp->Ready || !Dp->Link.Revision) {
    return -EINVAL;
  }

  if (!On && Dp->Streaming) {
    return -EBUSY;
  }

  if (Dp->Link.Revision < 0x11) {
    return 0;
  }

  Ret = SocDpDpcdRead (Dp, DP_SET_POWER, &Power, 1);
  if (Ret < 0) {
    return Ret;
  }

  Power = (Power & ~DP_SET_POWER_MASK) |
          (On ? DP_SET_POWER_D0 : DP_SET_POWER_D3);
  for (UINTN Attempt = 0; Attempt < (On ? 5U : 1U); Attempt++) {
    Ret = SocDpDpcdWriteb (Dp, DP_SET_POWER, Power);
    if (Ret >= 0) {
      if (On) {
        /* Sink exit from D3 is bounded to 1 ms in DP 1.1. Keep the
           existing 2 ms wake interval used by this transport. */
        MicroSecondDelay (2000);
      }

      return 0;
    }

    if (On) {
      MicroSecondDelay (2000);
    }
  }

  return Ret;
}

INTN
SocDpConnGetEdidBlock (
  IN SOC_DP_DEV  *Dp,
  OUT UINT8      *Buf,
  IN UINT32      Block,
  IN UINTN       Len
  )
{
  INTN                Ret, i;
  UINT8               Offset, Segment;
  UINT8               Data[EDID_LENGTH];
  UINT8               Checksum  = 0;
  static const UINT8  Header[8] = { 0, 255, 255, 255, 255, 255, 255, 0 };

  if (!Dp || !Dp->Ready || !Buf || (Block > 255) || (Len != EDID_LENGTH) || !Dp->Link.Revision) {
    return -EINVAL;
  }

  Ret = SocDpSinkPower (Dp, TRUE);
  if (Ret < 0) {
    return Ret;
  }

  Segment = Block / 2;
  if (Segment || Dp->EdidSegment) {
    Dp->EdidSegment = 255; /* A failed write may have changed the sink. */
    Ret             = SocDpAuxI2cWrite (Dp, 0x30, &Segment, 1);
    if (Ret < 0) {
      return Ret;
    }

    Dp->EdidSegment = Segment;
  }

  Offset = (UINT8)(Block * EDID_LENGTH) & 0xFF;

  Ret = SocDpAuxTransferWithRetry (Dp, DP_AUX_I2C_WRITE | DP_AUX_I2C_MOT, 0x50, &Offset, 1);

  if (Ret < 0) {
    DP_DEBUG (Dp, (DEBUG_ERROR, "[EDID] AUX write offset failed: %d\n", Ret));
    return Ret;
  }

  for (i = 0; i < (INT32)Len; i += 16) {
    Ret = SocDpAuxTransferWithRetry (
            Dp,
            DP_AUX_I2C_READ |
            (i + 16 < (INTN)Len ? DP_AUX_I2C_MOT : 0),
            0x50,
            Data + i,
            16
            );

    if (Ret < 0) {
      DP_DEBUG (Dp, (DEBUG_ERROR, "[EDID] AUX read data failed at offset %d: %d\n", i, Ret));
      SocDpAuxHwReset (Dp);
      return Ret;
    }
  }

  for (i = 0; i < EDID_LENGTH; i++) {
    Checksum += Data[i];
  }

  if (Checksum || (!Block && CompareMem (Data, Header, sizeof (Header)))) {
    return -EIO;
  }

  CopyMem (Buf, Data, EDID_LENGTH);

  return 0;
}

STATIC
INTN
UpdateEdpConfig (
  IN SOC_DP_DEV  *Dp,
  IN BOOLEAN     Enable
  )
{
  UINT8  Value;
  INTN   Ret;

  Ret = SocDpDpcdRead (Dp, DP_EDP_CONFIGURATION_SET, &Value, 1);
  if (Ret < 0) {
    DP_DEBUG (Dp, (DEBUG_ERROR, "Failed to read DP_EDP_CONFIGURATION_SET, ret: %d\n", Ret));
    return Ret;
  }

  if (Enable) {
    Value |= 0x01;
  } else {
    Value &= ~0x01;
  }

  Ret = SocDpDpcdWrite (Dp, DP_EDP_CONFIGURATION_SET, &Value, 1);
  if (Ret < 0) {
    DP_DEBUG (Dp, (DEBUG_ERROR, "Failed to write DP_EDP_CONFIGURATION_SET, ret: %d\n", Ret));
    return Ret;
  }

  return 0;
}

INTN
SocDpModeSet (
  IN SOC_DP_DEV               *Dp,
  IN CONST SOC_DP_VIDEO_MODE  *Mode
  )
{
  INTN                       i;
  INTN                       Ret = -ETIMEDOUT;
  UINTN                      ReqBw;
  INTN                       Bpp;
  CONST SOC_DP_LINK_CONFIG   *Cfg;
  SOC_DP_PHY_CONFIGURE_OPTS  PhyOpts;

  if (!Dp || !Dp->Ready || !SocDpModeValid (Mode) || Dp->CleanupError) {
    return -EINVAL;
  }

  Ret = SocDpHwDisable (Dp);
  if (Ret) {
    return Ret;
  }

  Ret = SocDpPhyPowerOff (&Dp->Phy);
  if (Ret) {
    Dp->CleanupError = Ret;
    return Ret;
  }

  if (!Dp->EdpMode && (SocDpHwDetectHpd (Dp) != ConnectorStatusConnected)) {
    return -ENOLINK;
  }

  Ret = SocDpHwReadSinkCaps (Dp);
  if (Ret) {
    return Ret;
  }

  SocDpRegWriteRange (Dp, SOC_DPTX_ENHANCE_FRAMING_EN, Dp->Link.EnhancedFraming);
  Ret = SocDpDpcdWriteb (Dp, DP_MAIN_LINK_CHANNEL_CODING_SET, DP_SET_ANSI_8B10B);
  if (Ret < 0) {
    return Ret;
  }

  Ret = -EOPNOTSUPP;

  DP_DEBUG (
    Dp,
    (
     DEBUG_INFO,
     "DP: Mode Set %dx%d (PCLK: %d kHz)\n",
     Mode->Hdisplay,
     Mode->Vdisplay,
     Mode->Clock
    )
    );
  ZeroMem (&PhyOpts, sizeof (PhyOpts));

  Bpp   = SocDpGetBpp (Dp->ColorFormat);
  ReqBw = SocDpCalcRequiredBw (Mode, Bpp);

  for (i = 0; i < (INT32)ARRAY_SIZE (SocDpLinkPriorityTable); i++) {
    UINTN  Capacity;

    Cfg = &SocDpLinkPriorityTable[i];

    /* Filter 1: Check HW Capabilities (Source & Sink limits) */
    if ((Cfg->Rate > Dp->Link.MaxRate) || (Cfg->Lanes > Dp->Link.MaxNumLanes)) {
      continue;
    }

    /* Filter 2: Check Bandwidth Requirement */
    Capacity = SocDpCalcLinkCapacity (Cfg->Rate, Cfg->Lanes);
    /* The six-bit integer TU field cannot represent a rounded TU of 64. */
    if ((Capacity < ReqBw) || (((UINT64)ReqBw * 800 + Cfg->Rate * Cfg->Lanes / 2) /
                               (Cfg->Rate * Cfg->Lanes) >= 640))
    {
      continue;
    }

    DP_DEBUG (
      Dp,
      (
       DEBUG_VERBOSE,
       "DP: Attempting Config: R=%d, L=%d (Cap: %d > Req: %d)\n",
       Cfg->Rate,
       Cfg->Lanes,
       Capacity,
       ReqBw
      )
      );

    /* Configure PHY Link parameters */
    PhyOpts.Lanes    = Cfg->Lanes;
    PhyOpts.LinkRate = Cfg->Rate / 1000;
    PhyOpts.SetLanes = 1;
    PhyOpts.SetRate  = 1;

    Ret = SocDpPhyPowerOff (&Dp->Phy);
    if (Ret) {
      Dp->CleanupError = Ret;
      return Ret;
    }

    Ret = SocDpPhyConfigure (&Dp->Phy, &PhyOpts);
    if (Ret != 0) {
      continue;
    }

    Ret = SocDpPhySetPixelClk (&Dp->Phy, Mode->Clock);
    if (Ret != 0) {
      continue;
    }

    Ret = SocDpPhyPowerOn (&Dp->Phy);
    if (Ret != 0) {
      continue;
    }

    if (Dp->EdpMode) {
      DP_DEBUG (Dp, (DEBUG_INFO, "%a eDP mode\n", __FUNCTION__));
      SocDpRegWriteRange (Dp, SOC_DPTX_ENABLE_EDP, 0x1);
      SocDpRegWriteRange (Dp, SOC_DPTX_STREAM_ENC_EN, 0x1);
      Ret = UpdateEdpConfig (Dp, TRUE);
      if (Ret) {
        continue;
      }
    }

    /* Execute Link Training */
    Ret = SocDpLinkTrain (Dp, Cfg->Rate, Cfg->Lanes);
    if (Ret == 0) {
      DP_DEBUG (
        Dp,
        (
         DEBUG_INFO,
         "DP: Training successful for R:%d L:%d\n",
         Cfg->Rate,
         Cfg->Lanes
        )
        );
      break;
    }

    DP_DEBUG (
      Dp,
      (
       DEBUG_WARN,
       "DP: Training failed for R:%d L:%d. Upgrading...\n",
       Cfg->Rate,
       Cfg->Lanes
      )
      );
  }

  if (Ret != 0) {
    DP_DEBUG (
      Dp,
      (
       DEBUG_WARN,
       "DP: No valid link configuration found for %dx%d\n",
       Mode->Hdisplay,
       Mode->Vdisplay
      )
      );
  }

  if (Ret != 0) {
    Dp->CleanupError = SocDpPhyPowerOff (&Dp->Phy);
    return Ret;
  }

  SocDpHwSetMsa (Dp, Mode, Dp->Phy.LinkRateKhz, Dp->Phy.LaneCount);
  Dp->VideoMode = *Mode;
  Dp->Trained   = TRUE;
  return 0;
}

INTN
SocDpHwRecoverLink (
  IN SOC_DP_DEV  *Dp
  )
{
  if (!Dp || !Dp->Ready || Dp->CleanupError ||
      !SocDpModeValid (&Dp->VideoMode))
  {
    return -EINVAL;
  }

  /* The caller retains the scanout buffer and unchanged DPU timing.
     Only the link and its clocks are restarted; no DMA page is freed. */
  SOC_DP_VIDEO_MODE  Mode = Dp->VideoMode;
  INTN               Ret  = SocDpModeSet (Dp, &Mode);

  return Ret ? Ret : SocDpHwEnable (Dp);
}

INTN
SocDpInit (
  IN SOC_DP_DEV        *Dp,
  IN UINTN             BaseAddr,
  IN SOC_DP_REF_CLK    RefClkKhz,
  IN SOC_VIDEO_FORMAT  ColorFormat
  )
{
  INTN  Ret;

  if (!Dp || !BaseAddr || (BaseAddr & 3) || (BaseAddr > MAX_UINTN - 0x4000) ||
      ((RefClkKhz != SOC_DP_REF_CLK_24M) && (RefClkKhz != SOC_DP_REF_CLK_50M)) ||
      ((UINTN)ColorFormat >= ARRAY_SIZE (FormatInfoTable)))
  {
    return -EINVAL;
  }

  ZeroMem (Dp, sizeof (*Dp));
  Dp->Regs        = BaseAddr;
  Dp->ColorFormat = ColorFormat;
  Dp->Ready       = TRUE;

  /* Reset Controller */
  SocDpRegWriteRange (Dp, SOC_DPTX_CONTROLLER_RESET, 0x1);
  SocDpRegWriteRange (Dp, SOC_DPTX_HDCP_RESET, 0x1);
  SocDpRegWriteRange (Dp, SOC_DPTX_AUX_RESET, 0x1);
  SocDpRegWriteRange (Dp, SOC_DPTX_VIDEO_RESET, 0x1);
  MicroSecondDelay (5000);

  /* Clear Video Reset */
  SocDpRegWriteRange (Dp, SOC_DPTX_CONTROLLER_RESET, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_HDCP_RESET, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_AUX_RESET, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_VIDEO_RESET, 0x0);
  MicroSecondDelay (2000);

  SocDpRegOnlyWriteRange (Dp, SOC_DPTX_AUX_REPLY_EVENT_INT_STA, 1);

  SocDpRegWriteRange (Dp, SOC_DPTX_DEFAULT_FAST_LINK_TRAIN_EN, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_SCRAMBLER_DISABLE, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_SCALE_DOWN_MODE, 0x0);

  /* Polling transport: disable controller interrupt sources. */
  SocDpRegWriteRange (Dp, SOC_DPTX_AUX_REPLY_EVENT_INT_STA_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_HDCP_INT_STA_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_ILLEGAL_AUX_CMD_INT_STA_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_TYPE_C_EVENT_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_DSC_EVENT_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_SDP_INT_STA_S3_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_SDP_INT_STA_S2_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_SDP_INT_STA_S1_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_SDP_INT_STA_S0_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_VIDEO_FIFO_OVERFLOW_INT_STA_S3_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_VIDEO_FIFO_OVERFLOW_INT_STA_S2_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_VIDEO_FIFO_OVERFLOW_INT_STA_S1_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_VIDEO_FIFO_OVERFLOW_INT_STA_S0_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_SINK_IRQ_EVENT_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_HPD_INT_STA_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_HOT_PLUG_EVENT_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_HOT_UNPLUG_EVENT_MSK, 0x0);
  SocDpRegWriteRange (Dp, SOC_DPTX_SINK_UNPLUG_ERROR_EVENT_MSK, 0x0);
  MicroSecondDelay (2000);

  SocDpRegWriteRange (Dp, SOC_DPTX_VIDEO_STREAM_ENABLE, 0);

  /* Initial PHY Config */
  Ret = SocDpPhyInit (&Dp->Phy, BaseAddr, RefClkKhz);
  if (Ret != 0) {
    DP_DEBUG (Dp, (DEBUG_ERROR, "Failed to init PHY\n"));
    SocDpExit (Dp);
    return Ret;
  }

  Ret = SocDpPhyPowerOff (&Dp->Phy);
  if (Ret) {
    SocDpExit (Dp);
  }

  return Ret;
}
