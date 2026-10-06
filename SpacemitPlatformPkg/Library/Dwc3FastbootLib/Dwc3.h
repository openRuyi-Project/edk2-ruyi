/** SPDX-License-Identifier: BSD-2-Clause-Patent
 * DWC3 register definitions checked against Intel's BSD-licensed XdciDWC.h
 * in Slim Bootloader f5f0b1cb00763d78d44eb328224459a219895939 and the
 * retained K3 U-Boot DWC3 driver. Addresses are relative to the full core.
 */
#pragma once
#define GCTL          0xc110
#define GSNPSID       0xc120
#define GUSB2PHYCFG   0xc200
#define GUSB3PIPECTL  0xc2c0
#define GEVNTADRLO    0xc400
#define GEVNTADRHI    0xc404
#define GEVNTSIZ      0xc408
#define GEVNTCOUNT    0xc40c
#define DCFG          0xc700
#define DCTL          0xc704
#define DEVTEN        0xc708
#define DSTS          0xc70c
#define DALEPENA      0xc720
#define DEPCMDPAR2(n)  (0xc800 + 16 * (n))
#define DEPCMDPAR1(n)  (0xc804 + 16 * (n))
#define DEPCMDPAR0(n)  (0xc808 + 16 * (n))
#define DEPCMD(n)      (0xc80c + 16 * (n))
#define CMD_ACTIVE    BIT10
#define CMD_STATUS    (0xfU << 12)
#define CMD_CONFIG    1
#define CMD_RESOURCE  2
#define CMD_STALL     4
#define CMD_UNSTALL   5
#define CMD_START     6
#define CMD_END       8
#define CMD_NEW       9
#define CORE_RESET    BIT30
#define RUN_STOP      BIT31
#define CORE_HALTED   BIT22
#define TRB_NORMAL    1
#define TRB_SETUP     2
#define TRB_STATUS2   3
#define TRB_STATUS3   4
#define TRB_DATA      5
#define TRB_HWO       BIT0
#define TRB_LAST      BIT1
#define TRB_ISP       BIT10
#define TRB_IOC       BIT11
#define EVENT_BYTES   EFI_PAGE_SIZE
#define BULK_BYTES    SIZE_16KB

typedef struct {
  UINT32    Low;
  UINT32    High;
  UINT32    Size;
  UINT32    Control;
  UINT8     Padding[48]; // One cache line per independent DMA ownership unit.
} DWC3_TRB;
