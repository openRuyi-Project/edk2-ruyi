/** @file
  Owned K3 Innosilicon DisplayPort transport and PHY interface.

  Copyright (c) 2025, SpacemiT Co., Ltd. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  The caller owns DT/MMIO mapping, clocks, power and resets before Init.
  Operations are serialized. INTN results use zero or byte count for
  success and negative errno for failure; use SocDpStatusToEfi at a UEFI
  boundary. No EFI_ERROR test may be applied to an INTN byte count.
  Neither training nor stream enable establishes DPU scanout.
**/

#ifndef K3_INNO_DP_LIB_H
#define K3_INNO_DP_LIB_H
#include <Uefi.h>

typedef enum {
  SOC_DP_LINK_RATE_1_62 = 1620000,
  SOC_DP_LINK_RATE_2_70 = 2700000,
  SOC_DP_LINK_RATE_5_40 = 5400000,
  SOC_DP_LINK_RATE_8_10 = 8100000,
} SOC_DP_LINK_RATE;

typedef enum {
  SOC_DP_LANE_1 = 1,
  SOC_DP_LANE_2 = 2,
  SOC_DP_LANE_4 = 4,
} SOC_DP_LANE_COUNT;

typedef struct {
  /* Serial bit rate in Mbit/s (1620/2700/5400); pixel clocks use kHz. */
  UINT32    LinkRate;
  UINT8     Lanes;

  UINT8     Voltage[4];
  UINT8     Pre[4];

  UINT8     SetRate;
  UINT8     SetLanes;
  UINT8     SetVoltages;
} SOC_DP_PHY_CONFIGURE_OPTS;

typedef struct {
  UINTN      Regs;
  UINT32     RefClkKhz;
  BOOLEAN    Quiet;

  /* Serial bit clock in kHz, ten times the DP symbol clock. */
  UINT32     LinkRateKhz;
  INTN       LaneCount;
} SOC_DP_PHY;

INTN
SocDpPhyInit (
  IN SOC_DP_PHY  *Phy,
  IN UINTN       BaseAddr,
  IN UINT32      RefClkKhz
  );

INTN
SocDpPhyExit (
  IN SOC_DP_PHY  *Phy
  );

INTN
SocDpPhyPowerOn (
  IN SOC_DP_PHY  *Phy
  );

INTN
SocDpPhyPowerOff (
  IN SOC_DP_PHY  *Phy
  );

INTN
SocDpPhyConfigure (
  IN SOC_DP_PHY                 *Phy,
  IN SOC_DP_PHY_CONFIGURE_OPTS  *Opts
  );

/* Read-only prediction using the same divider solver as programming. */
INTN
SocDpPhyRoundPixelClk (
  IN UINT32   RefClkKhz,
  IN UINT32   RequestedKhz,
  OUT UINT32  *ActualKhz
  );

INTN
SocDpPhySetPixelClk (
  IN SOC_DP_PHY  *Phy,
  IN UINT32      PixelClkKhz
  );

#define EDID_LENGTH           128
#define DP_RECEIVER_CAP_SIZE  0xF

#define SOC_DP_MODE_FLAG_PHSYNC  BIT0
#define SOC_DP_MODE_FLAG_PVSYNC  BIT3

typedef enum {
  SOC_VIDEO_RGB_6BIT     = 0,
  SOC_VIDEO_RGB_8BIT     = 1,
  SOC_VIDEO_RGB_10BIT    = 2,
  SOC_VIDEO_RGB_12BIT    = 3,
  SOC_VIDEO_RGB_16BIT    = 4,
  SOC_VIDEO_YUV444_8BIT  = 5,
  SOC_VIDEO_YUV444_10BIT = 6,
  SOC_VIDEO_YUV444_12BIT = 7,
  SOC_VIDEO_YUV444_16BIT = 8,
  SOC_VIDEO_YUV422_8BIT  = 9,
  SOC_VIDEO_YUV422_10BIT = 10,
  SOC_VIDEO_YUV422_12BIT = 11,
  SOC_VIDEO_YUV422_16BIT = 12,
} SOC_VIDEO_FORMAT;

typedef enum {
  SOC_DP_REF_CLK_24M = 24000,
  SOC_DP_REF_CLK_50M = 50000,
} SOC_DP_REF_CLK;

typedef enum {
  ConnectorStatusDisconnected = 0,
  ConnectorStatusConnected    = 1,
  ConnectorStatusUnknown      = 2,
} SOC_DP_CONNECTOR_STATUS;

typedef struct {
  /* Actual pixel clock in kHz, shared with the DPU timing source. */
  UINT32    Clock;
  UINT16    Hdisplay;
  UINT16    Htotal;
  UINT16    HsyncStart;
  UINT16    HsyncEnd;
  UINT16    Vdisplay;
  UINT16    Vtotal;
  UINT16    VsyncStart;
  UINT16    VsyncEnd;

  UINT32    Flags;
} SOC_DP_VIDEO_MODE;

typedef struct {
  SOC_DP_LINK_RATE     Rate;
  SOC_DP_LANE_COUNT    Lanes;
} SOC_DP_LINK_CONFIG;

typedef struct {
  UINTN                Regs;

  SOC_DP_PHY           Phy;

  UINT8                Dpcd[DP_RECEIVER_CAP_SIZE];

  SOC_DP_VIDEO_MODE    VideoMode;
  BOOLEAN              EdpMode;

  struct {
    UINT8     Revision;
    UINT8     EnhancedFraming;
    UINT32    MaxRate;
    UINT32    MaxNumLanes;
  } Link;

  UINT32                     ColorFormat;
  SOC_DP_CONNECTOR_STATUS    ConnectorStatus;
  // A published graphics console must not write raw UART diagnostics.
  BOOLEAN                    Quiet;
  BOOLEAN                    Ready;
  BOOLEAN                    Trained;
  BOOLEAN                    Streaming;
  UINT8                      EdidSegment;
  INTN                       CleanupError;
} SOC_DP_DEV;

INTN
SocDpInit (
  IN SOC_DP_DEV        *Dp,
  IN UINTN             BaseAddr,
  IN SOC_DP_REF_CLK    RefClkKhz,
  IN SOC_VIDEO_FORMAT  ColorFormat
  );

SOC_DP_CONNECTOR_STATUS
SocDpHwDetectHpd (
  IN SOC_DP_DEV  *Dp
  );

VOID
SocDpHwCleanHpd (
  IN SOC_DP_DEV  *Dp
  );

INTN
SocDpHwReadSinkCaps (
  IN SOC_DP_DEV  *Dp
  );

INTN
SocDpConnGetEdidBlock (
  IN SOC_DP_DEV  *Dp,
  OUT UINT8      *Buf,
  IN UINT32      Block,
  IN UINTN       Len
  );

INTN
SocDpModeSet (
  IN SOC_DP_DEV               *Dp,
  IN CONST SOC_DP_VIDEO_MODE  *Mode
  );

/* Retrain the last successful mode. Caller must retain the scanout
   buffer and controller timing while the pixel clock is restarted. */
INTN
SocDpHwRecoverLink (
  IN SOC_DP_DEV  *Dp
  );

INTN
SocDpHwEnable (
  IN SOC_DP_DEV  *Dp
  );

/* Inspect a trained link without disabling the stream or its PHY on
   failure. LinkStatus, when supplied, must hold six receiver bytes. */
INTN
SocDpHwCheckLink (
  IN SOC_DP_DEV  *Dp,
  OUT UINT8      *LinkStatus OPTIONAL
  );

/* Standard DPCD D0/D3 power request. Quiesce video before requesting D3.
   DPCD revisions older than 1.1 have no power request register. */
INTN
SocDpSinkPower (
  IN SOC_DP_DEV  *Dp,
  IN BOOLEAN     On
  );

INTN
SocDpHwDisable (
  IN SOC_DP_DEV  *Dp
  );

BOOLEAN
SocDpModeValid (
  IN CONST SOC_DP_VIDEO_MODE  *Mode
  );

EFI_STATUS
SocDpStatusToEfi (
  IN INTN  Status
  );

/* Disable stream and all PHY PLLs. Failure retains ownership; callers
   must not gate clocks or reuse resources until Exit succeeds. */
INTN
SocDpExit (
  IN SOC_DP_DEV  *Dp
  );

#endif
