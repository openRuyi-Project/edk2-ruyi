/* SPDX-License-Identifier: GPL-2.0-or-later */

/* DA9063 register decoding follows Linux rtc-da9063.c.
 * Copyright (C) 2013-2015 Dialog Semiconductor Ltd.
 * OpenCores I2C sequencing follows U-Boot ocores_i2c.c.
 * Copyright Peter Korsgaard <peter@korsgaard.com>
 * Copyright (C) 2020 SiFive, Inc.
 */
#include <PiDxe.h>
#include <Guid/FdtHob.h>
#include <Guid/RtPropertiesTable.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/RealTimeClockLib.h>
#include <Library/TimeBaseLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Library/UefiLib.h>

#define I2C_PRE_LOW    0x00
#define I2C_PRE_HIGH   0x04
#define I2C_CONTROL    0x08
#define I2C_DATA       0x0c
#define I2C_COMMAND    0x10
#define I2C_STATUS     0x10
#define I2C_ENABLE     BIT7
#define I2C_START      0x91
#define I2C_WRITE      0x11
#define I2C_READ       0x21
#define I2C_READ_LAST  0x29
#define I2C_STOP       0x41
#define I2C_BUSY       BIT6
#define I2C_LOST       BIT5
#define I2C_TIP        BIT1
#define I2C_NACK       BIT7

STATIC UINTN   mI2c;
STATIC UINT8   mAddress;
STATIC UINT16  mPrescale;

STATIC EFI_STATUS
Map (
  UINTN  Base
  )
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;
  EFI_STATUS                       Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Base, SIZE_4KB, EFI_MEMORY_UC);

  if (EFI_ERROR (Status)) {
    Status = gDS->GetMemorySpaceDescriptor (Base, &Desc);
    if (EFI_ERROR (Status) || (Desc.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) ||
        (Desc.BaseAddress > Base) || (Desc.Length < SIZE_4KB) || (Base - Desc.BaseAddress > Desc.Length - SIZE_4KB))
    {
      return EFI_UNSUPPORTED;
    }
  }

  return gDS->SetMemorySpaceAttributes (Base, SIZE_4KB, EFI_MEMORY_UC);
}

STATIC EFI_STATUS
Wait (
  UINT8    Mask,
  BOOLEAN  Ack
  )
{
  for (UINTN Retry = 0; Retry < 1000; Retry++) {
    UINT8  Value = MmioRead8 (mI2c + I2C_STATUS);
    if (Value & I2C_LOST) {
      return EFI_DEVICE_ERROR;
    }

    if (!(Value & Mask)) {
      return Ack && (Value & I2C_NACK) ? EFI_NO_RESPONSE : EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
Send (
  UINT8  Data,
  UINT8  Command
  )
{
  MmioWrite8 (mI2c + I2C_DATA, Data);
  MmioWrite8 (mI2c + I2C_COMMAND, Command);
  return Wait (I2C_TIP, TRUE);
}

STATIC EFI_STATUS
Read (
  UINT8  Register,
  UINT8  *Data,
  UINTN  Size
  )
{
  EFI_STATUS  Status = Send (mAddress << 1, I2C_START);

  if (!EFI_ERROR (Status)) {
    Status = Send (Register, I2C_WRITE);
  }

  if (!EFI_ERROR (Status)) {
    Status = Send ((mAddress << 1) | 1, I2C_START);
  }

  for (UINTN Index = 0; !EFI_ERROR (Status) && Index < Size; Index++) {
    MmioWrite8 (mI2c + I2C_COMMAND, Index == Size - 1 ? I2C_READ_LAST : I2C_READ);
    Status = Wait (I2C_TIP, FALSE);
    if (!EFI_ERROR (Status)) {
      Data[Index] = MmioRead8 (mI2c + I2C_DATA);
    }
  }

  MmioWrite8 (mI2c + I2C_COMMAND, I2C_STOP);
  EFI_STATUS  StopStatus = Wait (I2C_BUSY | I2C_TIP, FALSE);

  return EFI_ERROR (Status) ? Status : StopStatus;
}

EFI_STATUS EFIAPI
LibGetTime (
  EFI_TIME               *Time,
  EFI_TIME_CAPABILITIES  *Capabilities
  )
{
  EFI_TPL     Old;
  UINT8       Saved[3], Page, Data[6];
  EFI_STATUS  Status;

  if (!Time) {
    return EFI_INVALID_PARAMETER;
  }

  // Linux owns the shared I2C bus after ExitBootServices. Runtime access is
  // explicitly excluded from the runtime properties table.
  if (EfiAtRuntime ()) {
    return EFI_UNSUPPORTED;
  }

  if (!mI2c) {
    return EFI_DEVICE_ERROR;
  }

  Old = gBS->RaiseTPL (TPL_NOTIFY);
  if (MmioRead8 (mI2c + I2C_STATUS) & (I2C_BUSY | I2C_TIP)) {
    gBS->RestoreTPL (Old);
    return EFI_NOT_READY;
  }

  Saved[0] = MmioRead8 (mI2c + I2C_PRE_LOW);
  Saved[1] = MmioRead8 (mI2c + I2C_PRE_HIGH);
  Saved[2] = MmioRead8 (mI2c + I2C_CONTROL);
  MmioWrite8 (mI2c + I2C_CONTROL, 0);
  MmioWrite8 (mI2c + I2C_PRE_LOW, (UINT8)mPrescale);
  MmioWrite8 (mI2c + I2C_PRE_HIGH, (UINT8)(mPrescale >> 8));
  MmioWrite8 (mI2c + I2C_COMMAND, 1);
  MmioWrite8 (mI2c + I2C_CONTROL, I2C_ENABLE);
  Status = Read (0, &Page, 1);
  // Do not modify PMIC state if another owner has selected a different page.
  if (!EFI_ERROR (Status)) {
    Status = (Page & BIT0) ? EFI_NOT_READY : Read (0x40, Data, sizeof (Data));
  }

  MmioWrite8 (mI2c + I2C_CONTROL, 0);
  MmioWrite8 (mI2c + I2C_PRE_LOW, Saved[0]);
  MmioWrite8 (mI2c + I2C_PRE_HIGH, Saved[1]);
  MmioWrite8 (mI2c + I2C_CONTROL, Saved[2]);
  gBS->RestoreTPL (Old);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!(Data[0] & BIT7)) {
    return EFI_NOT_READY;
  }

  ZeroMem (Time, sizeof (*Time));
  Time->Second   = Data[0] & 0x3f;
  Time->Minute   = Data[1] & 0x3f;
  Time->Hour     = Data[2] & 0x1f;
  Time->Day      = Data[3] & 0x1f;
  Time->Month    = Data[4] & 0xf;
  Time->Year     = 2000 + (Data[5] & 0x3f);
  Time->TimeZone = EFI_UNSPECIFIED_TIMEZONE;
  if (!IsTimeValid (Time)) {
    return EFI_DEVICE_ERROR;
  }

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
Configure (
  CONST VOID  *Fdt
  )
{
  INT32         Pmic, Bus, Clock, Reference, Length;
  CONST UINT32  *Data;
  UINTN         Prci, Base;
  UINT32        RefHz, Config, Divider;
  UINT64        Rate;
  EFI_STATUS    Status;

  Pmic = FdtNodeOffsetByCompatible (Fdt, -1, "dlg,da9063");
  if (Pmic < 0) {
    return EFI_NOT_FOUND;
  }

  Data = FdtGetProp (Fdt, Pmic, "reg", &Length);
  if (!Data || (Length != 4) || (Fdt32ToCpu (*Data) > 0x7f)) {
    return EFI_COMPROMISED_DATA;
  }

  mAddress = (UINT8)Fdt32ToCpu (*Data);
  Bus      = FdtParentOffset (Fdt, Pmic);
  if ((Bus < 0) || (FdtStringListSearch (Fdt, Bus, "compatible", "sifive,fu740-c000-i2c") < 0)) {
    return EFI_UNSUPPORTED;
  }

  Data = FdtGetProp (Fdt, Bus, "reg-shift", &Length);
  if (!Data || (Length != 4) || (Fdt32ToCpu (*Data) != 2)) {
    return EFI_UNSUPPORTED;
  }

  Data = FdtGetProp (Fdt, Bus, "reg-io-width", &Length);
  if (!Data || (Length != 4) || (Fdt32ToCpu (*Data) != 1)) {
    return EFI_UNSUPPORTED;
  }

  Data = FdtGetProp (Fdt, Bus, "reg", &Length);
  if (!Data || (Length != 16) || Data[0] || Data[2] || (Fdt32ToCpu (Data[3]) < SIZE_4KB)) {
    return EFI_COMPROMISED_DATA;
  }

  Base = Fdt32ToCpu (Data[1]);
  if (Base & EFI_PAGE_MASK) {
    return EFI_COMPROMISED_DATA;
  }

  Data = FdtGetProp (Fdt, Bus, "clocks", &Length);
  if (!Data || (Length != 8) || (Fdt32ToCpu (Data[1]) != 7)) {
    return EFI_UNSUPPORTED;
  }

  Clock = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Data));
  if ((Clock < 0) || (FdtStringListSearch (Fdt, Clock, "compatible", "sifive,fu740-c000-prci") < 0)) {
    return EFI_UNSUPPORTED;
  }

  Data = FdtGetProp (Fdt, Clock, "reg", &Length);
  if (!Data || (Length != 16) || Data[0] || Data[2] || (Fdt32ToCpu (Data[3]) < SIZE_4KB)) {
    return EFI_COMPROMISED_DATA;
  }

  Prci = Fdt32ToCpu (Data[1]);
  if (Prci & EFI_PAGE_MASK) {
    return EFI_COMPROMISED_DATA;
  }

  Data = FdtGetProp (Fdt, Clock, "clocks", &Length);
  if (!Data || (Length < 4)) {
    return EFI_COMPROMISED_DATA;
  }

  Reference = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Data));
  if (Reference < 0) {
    return EFI_COMPROMISED_DATA;
  }

  Data = FdtGetProp (Fdt, Reference, "clock-frequency", &Length);
  if (!Data || (Length != 4) || !*Data) {
    return EFI_COMPROMISED_DATA;
  }

  RefHz  = Fdt32ToCpu (*Data);
  Status = Map (Prci);
  if (!EFI_ERROR (Status)) {
    Status = Map (Base);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Config = MmioRead32 (Prci + 0x50);
  Rate   = RefHz;
  if (!(MmioRead32 (Prci + 0x58) & BIT0) && !(Config & BIT24)) {
    Rate = DivU64x32 (
             MultU64x32 (RefHz, 2 * (((Config >> 6) & 0x1ff) + 1)),
             ((Config & 0x3f) + 1) * (1U << ((Config >> 15) & 7))
             );
  }

  Divider = MmioRead32 (Prci + 0x5c);
  if (Divider > MAX_UINT32 - 2) {
    return EFI_DEVICE_ERROR;
  }

  Rate = DivU64x32 (Rate, Divider + 2);
  Rate = DivU64x32 (Rate + 500000 - 1, 500000); // PCLK / (5 * 100 kHz)
  if (!Rate || (Rate > 65536)) {
    return EFI_UNSUPPORTED;
  }

  mPrescale = (UINT16)(Rate - 1);
  mI2c      = Base;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
LibRtcInitialize (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE               *Hob   = GetFirstGuidHob (&gFdtHobGuid);
  EFI_STATUS                      Status = EFI_NOT_FOUND;
  EFI_TIME                        Time;
  EFI_RT_PROPERTIES_TABLE         *Properties;
  STATIC EFI_RT_PROPERTIES_TABLE  Defaults = {
    EFI_RT_PROPERTIES_TABLE_VERSION, sizeof (EFI_RT_PROPERTIES_TABLE), EFI_RT_SUPPORTED_GET_VARIABLE | EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME |
    EFI_RT_SUPPORTED_SET_VARIABLE | EFI_RT_SUPPORTED_SET_VIRTUAL_ADDRESS_MAP |
    EFI_RT_SUPPORTED_CONVERT_POINTER | EFI_RT_SUPPORTED_GET_NEXT_HIGH_MONOTONIC_COUNT |
    EFI_RT_SUPPORTED_RESET_SYSTEM | EFI_RT_SUPPORTED_UPDATE_CAPSULE |
    EFI_RT_SUPPORTED_QUERY_CAPSULE_CAPABILITIES | EFI_RT_SUPPORTED_QUERY_VARIABLE_INFO
  };

  if (Hob && (GET_GUID_HOB_DATA_SIZE (Hob) >= sizeof (UINT64))) {
    CONST VOID  *Fdt = (VOID *)(UINTN)ReadUnaligned64 (GET_GUID_HOB_DATA (Hob));
    if (Fdt && !FdtCheckHeader (Fdt)) {
      Status = Configure (Fdt);
    }
  }

  if (!EFI_ERROR (Status)) {
    Status = LibGetTime (&Time, NULL);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DA9063 RTC read: %r\n", Status));
  } else {
    DEBUG ((DEBUG_INFO, "DA9063 RTC: %04u-%02u-%02u %02u:%02u:%02u\n", Time.Year, Time.Month, Time.Day, Time.Hour, Time.Minute, Time.Second));
  }

  if (EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiRtPropertiesTableGuid, (VOID **)&Properties))) {
    Properties = &Defaults;
    Status     = gBS->InstallConfigurationTable (&gEfiRtPropertiesTableGuid, Properties);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Properties->RuntimeServicesSupported &= ~(EFI_RT_SUPPORTED_GET_TIME | EFI_RT_SUPPORTED_SET_TIME |
                                            EFI_RT_SUPPORTED_GET_WAKEUP_TIME | EFI_RT_SUPPORTED_SET_WAKEUP_TIME);
  // Publish the architecture protocol even if the RTC is unavailable.
  return EFI_SUCCESS;
}
