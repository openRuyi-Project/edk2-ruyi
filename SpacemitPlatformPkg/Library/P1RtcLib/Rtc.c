/* SPDX-License-Identifier: GPL-2.0 */
/* P1 calendar decoding follows Linux rtc-spacemit-p1.c.
 * Copyright (C) 2025 RISCstar Solutions Corporation.
 * Controller access uses the existing K3 polled I2C implementation.
 */
#include <PiDxe.h>
#include <Guid/FdtHob.h>
#include <Guid/RtPropertiesTable.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/K3HardwareLib.h>
#include <Library/K3I2cLib.h>
#include <Library/RealTimeClockLib.h>
#include <Library/TimeBaseLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeLib.h>

#define P1_RTC_TIME         0x0d
#define P1_RTC_CONTROL      0x1d
#define P1_RTC_ENABLE       BIT2
#define RTC_READ_TRIES      20

#define TWSI8_BASE          0xd401d800
#define TWSI8_CLOCK         0x20
#define TWSI8_FUNC_ID       79
#define TWSI8_BUS_ID        86
#define TWSI8_RESET_ID      41
#define TWSI_CLOCK_MUX      (7U << 4)
#define TWSI_RESET          BIT2
#define TWSI_CLOCK_ENABLE   (BIT0 | BIT1)
#define PLL1_CONFIG         0x100
#define PLL1_CONTROL        0x104
#define PLL1_CONFIG2        0x108
#define PLL1_DIV4_ENABLE    BIT3
#define MPMU_STATUS         0x10
#define PLL1_LOCKED         BIT24
#define MPMU_GATES          0x1024
#define PLL1_DIV78_ENABLE   BIT6

#define ICR                0x00
#define ISR                0x04
#define ISAR               0x08
#define ILCR               0x10
#define IWCR               0x14
#define IRCR               0x18
#define IBMR               0x1c
#define UNIT_RESET         BIT10
#define UNIT_ENABLE        (BIT13 | BIT14 | BIT21)
#define ACTIVE_CONTROL     (BIT0 | BIT1 | BIT3 | BIT12)
#define INTERRUPTS         (BIT18 | BIT19 | BIT20 | BIT22 | BIT23 | BIT24)
#define EVENTS             (BIT18 | BIT19 | BIT20 | BIT22)
#define SDA_GLITCH_BYPASS   BIT7

STATIC struct {
  UINTN    Base, Apbc, Pll, Mpmu;
  UINT8    Address;
  BOOLEAN  Ready;
} mBus;

STATIC EFI_STATUS
ReadCalendar (
  UINT8  Data[6]
  )
{
  UINT32  SavedClock = MmioRead32 (mBus.Apbc + TWSI8_CLOCK);
  UINT32  SavedPll   = MmioRead32 (mBus.Pll + PLL1_CONTROL);
  UINT32  SavedGate  = MmioRead32 (mBus.Mpmu + MPMU_GATES);
  UINT32  SavedIcr, SavedIsar, SavedIlcr, SavedIwcr, SavedIrcr;
  UINT8       Control, Previous[6];
  BOOLEAN     Owned = FALSE;
  EFI_STATUS  Status;

  // Use the initialized PLL1 / 78 path without retuning shared clocks.
  if ((SavedClock & TWSI_CLOCK_MUX) ||
      (MmioRead32 (mBus.Pll + PLL1_CONFIG) != 0x0b330ccc) ||
      (MmioRead32 (mBus.Pll + PLL1_CONFIG2) != 0xa0558989) ||
      !(MmioRead32 (mBus.Mpmu + MPMU_STATUS) & PLL1_LOCKED))
  {
    return EFI_NOT_READY;
  }

  MmioOr32 (mBus.Pll + PLL1_CONTROL, PLL1_DIV4_ENABLE);
  MmioOr32 (mBus.Mpmu + MPMU_GATES, PLL1_DIV78_ENABLE);
  MmioWrite32 (mBus.Apbc + TWSI8_CLOCK,
               (SavedClock & ~TWSI_RESET) | TWSI_CLOCK_ENABLE);
  MicroSecondDelay (10);

  SavedIcr  = MmioRead32 (mBus.Base + ICR);
  SavedIsar = MmioRead32 (mBus.Base + ISAR);
  SavedIlcr = MmioRead32 (mBus.Base + ILCR);
  SavedIwcr = MmioRead32 (mBus.Base + IWCR);
  SavedIrcr = MmioRead32 (mBus.Base + IRCR);
  if ((SavedIcr & (INTERRUPTS | ACTIVE_CONTROL)) ||
      EFI_ERROR (K3I2cWaitIdle (mBus.Base)) ||
      ((MmioRead32 (mBus.Base + IBMR) & 3) != 3))
  {
    Status = EFI_ACCESS_DENIED;
    goto Restore;
  }

  Owned = TRUE;
  MmioWrite32 (mBus.Base + ICR, UNIT_RESET);
  MicroSecondDelay (10);
  MmioWrite32 (mBus.Base + IRCR, SavedIrcr | SDA_GLITCH_BYPASS);
  MmioWrite32 (mBus.Base + ICR, UNIT_ENABLE);
  Status = K3I2cReadRegisters (mBus.Base, mBus.Address, P1_RTC_CONTROL,
                              &Control, sizeof (Control));
  if (EFI_ERROR (Status)) {
    goto Restore;
  }

  if (!(Control & P1_RTC_ENABLE)) {
    Status = EFI_NOT_READY;
    goto Restore;
  }

  Status = K3I2cReadRegisters (mBus.Base, mBus.Address, P1_RTC_TIME,
                              Previous, sizeof (Previous));
  if (EFI_ERROR (Status)) {
    goto Restore;
  }

  // P1's snapshot latch is unreliable. Require two matching calendars.
  for (UINTN Attempt = 0; Attempt < RTC_READ_TRIES; Attempt++) {
    Status = K3I2cReadRegisters (mBus.Base, mBus.Address, P1_RTC_TIME, Data, 6);
    if (EFI_ERROR (Status) || !CompareMem (Previous, Data, 6)) {
      goto Restore;
    }

    CopyMem (Previous, Data, 6);
  }

  Status = EFI_DEVICE_ERROR;

Restore:
  if (Owned) {
    MmioWrite32 (mBus.Base + ICR, UNIT_RESET);
    MicroSecondDelay (10);
    MmioWrite32 (mBus.Base + ICR, 0);
    if (EFI_ERROR (K3I2cWaitIdle (mBus.Base))) {
      DEBUG ((DEBUG_ERROR, "P1 RTC: I2C cleanup failed; clocks retained\n"));
      return EFI_DEVICE_ERROR;
    }

    MmioWrite32 (mBus.Base + ISR, EVENTS);
    MmioWrite32 (mBus.Base + ISAR, SavedIsar);
    MmioWrite32 (mBus.Base + ILCR, SavedIlcr);
    MmioWrite32 (mBus.Base + IWCR, SavedIwcr);
    MmioWrite32 (mBus.Base + IRCR, SavedIrcr);
    MmioWrite32 (mBus.Base + ICR, SavedIcr);
  }

  MmioWrite32 (mBus.Apbc + TWSI8_CLOCK, SavedClock);
  MmioAndThenOr32 (mBus.Mpmu + MPMU_GATES, ~PLL1_DIV78_ENABLE,
                   SavedGate & PLL1_DIV78_ENABLE);
  MmioAndThenOr32 (mBus.Pll + PLL1_CONTROL, ~PLL1_DIV4_ENABLE,
                   SavedPll & PLL1_DIV4_ENABLE);
  return Status;
}

EFI_STATUS EFIAPI
LibGetTime (
  EFI_TIME               *Time,
  EFI_TIME_CAPABILITIES  *Capabilities
  )
{
  if (EfiAtRuntime ()) {
    return EFI_UNSUPPORTED;
  }

  if (!Time) {
    return EFI_INVALID_PARAMETER;
  }

  if (!mBus.Ready) {
    return EFI_DEVICE_ERROR;
  }

  UINT8       Data[6];
  EFI_TPL     Old = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  Status = ReadCalendar (Data);

  gBS->RestoreTPL (Old);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  EFI_TIME  Calendar = {
    .Year     = 2000 + (Data[5] & 0x3f),
    .Month    = (Data[4] & 0x0f) + 1,
    .Day      = (Data[3] & 0x1f) + 1,
    .Hour     = Data[2] & 0x1f,
    .Minute   = Data[1] & 0x3f,
    .Second   = Data[0] & 0x3f,
    .TimeZone = EFI_UNSPECIFIED_TIMEZONE
  };
  if (!IsTimeValid (&Calendar)) {
    return EFI_DEVICE_ERROR;
  }

  *Time = Calendar;
  if (Capabilities) {
    Capabilities->Resolution = 1;
    Capabilities->Accuracy   = 0;
    Capabilities->SetsToZero = FALSE;
  }

  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
LibSetTime (
  EFI_TIME  *Time
  )
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS EFIAPI
LibGetWakeupTime (
  BOOLEAN   *Enabled,
  BOOLEAN   *Pending,
  EFI_TIME  *Time
  )
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS EFIAPI
LibSetWakeupTime (
  BOOLEAN   Enabled,
  EFI_TIME  *Time
  )
{
  return EFI_UNSUPPORTED;
}

STATIC EFI_STATUS
Region (
  CONST VOID   *Fdt,
  INT32        Node,
  CONST CHAR8  *Compatible,
  UINTN        Minimum,
  UINTN        *Base
  )
{
  if (!K3FdtEnabled (Fdt, Node) ||
      (FdtStringListSearch (Fdt, Node, "compatible", Compatible) < 0))
  {
    return EFI_UNSUPPORTED;
  }

  EFI_STATUS  Status = K3FdtRegister (Fdt, Node, 0, Minimum, Base, NULL);
  return EFI_ERROR (Status) ? Status : K3MapMmio (*Base, Minimum);
}

STATIC EFI_STATUS
Configure (
  CONST VOID  *Fdt
  )
{
  INT32         Pmic = -1, Bus, Clock, Pll, Length;
  CONST UINT32  *Data;
  EFI_STATUS    Status;

  do {
    Pmic = FdtNodeOffsetByCompatible (Fdt, Pmic, "spacemit,p1");
  } while ((Pmic >= 0) && !K3FdtEnabled (Fdt, Pmic));
  if (Pmic < 0) {
    return EFI_NOT_FOUND;
  }

  Data = FdtGetProp (Fdt, Pmic, "reg", &Length);
  if (!Data || (Length != 4) || !Fdt32ToCpu (*Data) ||
      (Fdt32ToCpu (*Data) > 0x7f))
  {
    return EFI_COMPROMISED_DATA;
  }

  mBus.Address = (UINT8)Fdt32ToCpu (*Data);
  Bus = FdtParentOffset (Fdt, Pmic);
  Status = Region (Fdt, Bus, "spacemit,k3-i2c", 0x20, &mBus.Base);
  if (EFI_ERROR (Status) || (mBus.Base != TWSI8_BASE)) {
    return EFI_UNSUPPORTED;
  }

  Data = FdtGetProp (Fdt, Bus, "clocks", &Length);
  if (!Data || (Length != 16) || (Data[0] != Data[2]) ||
      (Fdt32ToCpu (Data[1]) != TWSI8_FUNC_ID) ||
      (Fdt32ToCpu (Data[3]) != TWSI8_BUS_ID))
  {
    return EFI_UNSUPPORTED;
  }

  Clock = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Data[0]));
  Data = FdtGetProp (Fdt, Bus, "resets", &Length);
  if (!Data || (Length != 8) || (Fdt32ToCpu (Data[1]) != TWSI8_RESET_ID) ||
      (FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Data[0])) != Clock))
  {
    return EFI_UNSUPPORTED;
  }

  Status = Region (Fdt, Clock, "spacemit,k3-syscon-apbc",
                    TWSI8_CLOCK + sizeof (UINT32), &mBus.Apbc);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Pll = FdtNodeOffsetByCompatible (Fdt, -1, "spacemit,k3-pll");
  Status = Region (Fdt, Pll, "spacemit,k3-pll",
                    PLL1_CONFIG2 + sizeof (UINT32), &mBus.Pll);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Data = FdtGetProp (Fdt, Pll, "spacemit,mpmu", &Length);
  if (!Data || (Length != 4)) {
    return EFI_COMPROMISED_DATA;
  }

  return Region (Fdt, FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Data)),
                  "spacemit,k3-syscon-mpmu", MPMU_GATES + sizeof (UINT32),
                  &mBus.Mpmu);
}

EFI_STATUS EFIAPI
LibRtcInitialize (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE               *Hob = GetFirstGuidHob (&gFdtHobGuid);
  EFI_STATUS                      Status = EFI_NOT_FOUND;
  EFI_TIME                        Time;
  EFI_RT_PROPERTIES_TABLE         *Properties;
  STATIC EFI_RT_PROPERTIES_TABLE  Defaults = {
    EFI_RT_PROPERTIES_TABLE_VERSION, sizeof (EFI_RT_PROPERTIES_TABLE),
    EFI_RT_SUPPORTED_GET_VARIABLE | EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME |
    EFI_RT_SUPPORTED_SET_VARIABLE | EFI_RT_SUPPORTED_SET_VIRTUAL_ADDRESS_MAP |
    EFI_RT_SUPPORTED_CONVERT_POINTER | EFI_RT_SUPPORTED_GET_NEXT_HIGH_MONOTONIC_COUNT |
    EFI_RT_SUPPORTED_RESET_SYSTEM | EFI_RT_SUPPORTED_UPDATE_CAPSULE |
    EFI_RT_SUPPORTED_QUERY_CAPSULE_CAPABILITIES | EFI_RT_SUPPORTED_QUERY_VARIABLE_INFO
  };

  if (Hob && (GET_GUID_HOB_DATA_SIZE (Hob) >= sizeof (UINT64))) {
    CONST VOID  *Fdt = (VOID *)(UINTN)ReadUnaligned64 (GET_GUID_HOB_DATA (Hob));
    if (Fdt && !FdtCheckHeader (Fdt) && (FdtTotalSize (Fdt) <= SIZE_1MB)) {
      Status = Configure (Fdt);
    }
  }

  mBus.Ready = !EFI_ERROR (Status);
  if (mBus.Ready) {
    Status = LibGetTime (&Time, NULL);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "P1 RTC read: %r\n", Status));
  } else {
    DEBUG ((DEBUG_INFO, "P1 RTC: %04u-%02u-%02u %02u:%02u:%02u\n",
            Time.Year, Time.Month, Time.Day, Time.Hour, Time.Minute, Time.Second));
  }

  if (EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiRtPropertiesTableGuid,
                                                 (VOID **)&Properties))) {
    Properties = &Defaults;
    Status = gBS->InstallConfigurationTable (&gEfiRtPropertiesTableGuid, Properties);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Properties->RuntimeServicesSupported &= ~(EFI_RT_SUPPORTED_GET_TIME |
                                            EFI_RT_SUPPORTED_SET_TIME |
                                            EFI_RT_SUPPORTED_GET_WAKEUP_TIME |
                                            EFI_RT_SUPPORTED_SET_WAKEUP_TIME);
  // Keep the architecture protocol available even if the RTC read fails.
  return EFI_SUCCESS;
}
