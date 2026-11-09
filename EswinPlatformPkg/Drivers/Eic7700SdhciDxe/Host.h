/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <PiDxe.h>
#include <Protocol/SdMmcOverride.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtBoardInfoLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define EIC_MMC_REGISTER_SIZE  SIZE_64KB

#define HC_HOST_CTRL         0x28
#define HC_CLOCK_CTRL        0x2c
#define HC_RESET             0x2f
#define HC_INT_STATUS        0x30
#define HC_ERROR_STATUS      0x32
#define HC_HOST_CTRL2        0x3e
#define HC_VENDOR_POINTER    0xe8
#define HC_CARD_CLOCK        BIT2
#define HC_CD_TEST_SELECT    BIT7
#define HC_CD_TEST_PRESENT   BIT6
#define HC_CLOCK_DIV_MASK    0xffc0U
#define HC_EXECUTE_TUNING    BIT6
#define HC_TUNED_CLOCK       BIT7
#define HC_PRESET_ENABLE     BIT15
#define HC_RESET_CMD_DATA    (BIT1 | BIT2)
#define HC_XFER_COMPLETE     BIT1
#define HC_ERROR             BIT15
#define DWC_HOST_CTRL3       0x08
#define DWC_EMMC_CTRL        0x2c
#define DWC_CARD_IS_EMMC     BIT0
#define DWC_AT_CTRL          0x40
#define DWC_AT_PHASE         0x44
#define DWC_SW_TUNING        BIT4
#define DWC_POST_DELAY_MASK  (3U << 19)
#define PHY_CONFIG           0x300
#define PHY_RESET_N          BIT0
#define PHY_POWER_GOOD       BIT1
#define PHY_DRIVE_MASK       (0xffU << 16)
#define PHY_CMD_PAD          0x304
#define PHY_DATA_PAD         0x306
#define PHY_CLOCK_PAD        0x308
#define PHY_STROBE_PAD       0x30a
#define PHY_RESET_PAD        0x30c
#define PHY_RX_1V8           1
#define PHY_PULL_UP          BIT3
#define PHY_SLEW             ((2U << 5) | (2U << 9))
#define PHY_CLOCK_DELAY_CFG  0x31d
#define PHY_CLOCK_DELAY      0x31e
#define PHY_DELAY_UPDATE     BIT4
#define PHY_DLL_CONTROL      0x324
#define PHY_DLL_CONFIG1      0x325
#define PHY_DLL_CONFIG2      0x326
#define PHY_DLL_DELAY        0x328
#define PHY_DLL_OFFSET       0x329
#define PHY_DLL_TIMER        0x32c
#define PHY_DLL_STATUS       0x32e
#define PHY_DLL_LOCK         BIT0
#define PHY_DLL_ERROR        BIT1
#define EIC_DMA_BOUNDARY      SIZE_128MB

typedef struct {
  EFI_HANDLE  Handle;
  UINTN       Base, Clock, Reset, Hsp, Vendor;
  UINTN       ClockControl, HspStable, HspPower;
  UINTN       GpioBase, GpioBank, GpioBit;
  UINT32      ResetMask, MaxClock, Drive;
  UINT8       Width, Delay;
  BOOLEAN     Emmc, Hs400, ActiveLow, Ready;
} EIC_MMC_PORT;

extern EIC_MMC_PORT  mPorts[2];
extern UINTN         mPortCount;

EIC_MMC_PORT *PortFor (EFI_HANDLE Handle, UINT8 Slot);
EFI_STATUS ParsePort (CONST VOID *Fdt, INT32 Node, EIC_MMC_PORT *Port);
EFI_STATUS InitializePort (EIC_MMC_PORT *Port);
EFI_STATUS ConfigurePhy (EIC_MMC_PORT *Port);
EFI_STATUS SetCardClock (EIC_MMC_PORT *Port, UINT32 Hertz);
EFI_STATUS SetDll (EIC_MMC_PORT *Port);
VOID SetDelay (EIC_MMC_PORT *Port, UINT8 Delay);
VOID RefreshCardDetect (EIC_MMC_PORT *Port);
EFI_STATUS ResetCommandData (EIC_MMC_PORT *Port);
EFI_STATUS EFIAPI ExecuteTuning (EFI_HANDLE Handle, UINT8 Slot,
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru, BOOLEAN IsEmmc, UINT8 BusWidth);
