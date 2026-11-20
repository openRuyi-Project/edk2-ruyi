/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/CpuMmioAddressLib.h>
#include <PiDxe.h>
#include <Guid/RtPropertiesTable.h>
#include <Guid/EventGroup.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/TimeBaseLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeLib.h>

#define IC_CON           0x00
#define IC_TAR           0x04
#define IC_DATA_CMD      0x10
#define IC_RAW_INTR      0x34
#define IC_CLR_INTR      0x40
#define IC_ENABLE        0x6c
#define IC_STATUS        0x70
#define IC_ENABLE_STATUS 0x9c
STATIC UINTN   mBus;
STATIC UINTN   mClockGate;
STATIC BOOLEAN mRuntimeOwner;
STATIC BOOLEAN mVirtual;
STATIC UINT32 mAddress;

STATIC EFI_STATUS
Wait (UINTN Base, UINTN Reg, UINT32 Mask, UINT32 Value)
{
  for (UINTN I = 0; I < 1000000; I++) {
    if ((MmioRead32 (Base + Reg) & Mask) == Value) { return EFI_SUCCESS; }
    if (Reg != IC_ENABLE_STATUS && (MmioRead32 (Base + IC_RAW_INTR) & BIT6)) { return EFI_DEVICE_ERROR; }
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
Transfer (BOOLEAN Read, UINT8 Data[7])
{
  EFI_STATUS Status;
  BOOLEAN AtRuntime = EfiAtRuntime ();
  EFI_TPL Old = TPL_APPLICATION;
  UINTN Base = mVirtual ? mBus : CpuMmioAddress (mBus);

  if (AtRuntime && !mRuntimeOwner) {
    return EFI_UNSUPPORTED;
  }
  // Runtime callers serialize time services. TPL applies only before exit.
  if (!AtRuntime) {
    Old = gBS->RaiseTPL (TPL_NOTIFY);
  }
  if ((MmioRead32 (mVirtual ? mClockGate : CpuMmioAddress (mClockGate)) & BIT26) == 0 ||
      (MmioRead32 (Base + IC_STATUS) & BIT0) != 0) {
    Status = EFI_NOT_READY;
    goto Done;
  }
  UINT32 Enabled = MmioRead32 (Base + IC_ENABLE) & 1;
  UINT32 Control = MmioRead32 (Base + IC_CON);
  UINT32 Target = MmioRead32 (Base + IC_TAR);
  MmioWrite32 (Base + IC_ENABLE, 0);
  Status = Wait (Base, IC_ENABLE_STATUS, 1, 0);
  if (!EFI_ERROR (Status)) {
    MmioRead32 (Base + IC_CLR_INTR);
    // Standard mode uses the hardware's standard-speed SCL counts.
    MmioWrite32 (Base + IC_CON, BIT0 | BIT1 | BIT5 | BIT6);
    MmioWrite32 (Base + IC_TAR, mAddress);
    MmioWrite32 (Base + IC_ENABLE, 1);
    Status = Wait (Base, IC_ENABLE_STATUS, 1, 1);
  }
  if (!EFI_ERROR (Status)) {
    MmioWrite32 (Base + IC_DATA_CMD, 0); // DS1307 seconds register
    for (UINTN I = 0; I < 7; I++) {
      Status = Wait (Base, IC_STATUS, BIT1, BIT1);
      if (EFI_ERROR (Status)) { break; }
      UINT32 Cmd = Read ? BIT8 : Data[I];
      if (Read && I == 0) { Cmd |= BIT10; }
      if (I == 6) { Cmd |= BIT9; }
      MmioWrite32 (Base + IC_DATA_CMD, Cmd);
      if (Read) {
        Status = Wait (Base, IC_STATUS, BIT3, BIT3);
        if (EFI_ERROR (Status)) { break; }
        Data[I] = (UINT8)MmioRead32 (Base + IC_DATA_CMD);
      }
    }
    if (!EFI_ERROR (Status)) { Status = Wait (Base, IC_RAW_INTR, BIT9, BIT9); }
    if (MmioRead32 (Base + IC_RAW_INTR) & BIT6) { Status = EFI_DEVICE_ERROR; }
  }
  MmioWrite32 (Base + IC_ENABLE, 0);
  EFI_STATUS Stopped = Wait (Base, IC_ENABLE_STATUS, 1, 0);
  if (!EFI_ERROR (Stopped)) {
    MmioRead32 (Base + IC_CLR_INTR);
    MmioWrite32 (Base + IC_CON, Control);
    MmioWrite32 (Base + IC_TAR, Target);
    MmioWrite32 (Base + IC_ENABLE, Enabled);
  }
  if (!EFI_ERROR (Status)) {
    Status = Stopped;
  }
Done:
  if (!AtRuntime) {
    gBS->RestoreTPL (Old);
  }
  return Status;
}

EFI_STATUS EFIAPI
LibGetTime (EFI_TIME *Time, EFI_TIME_CAPABILITIES *Caps)
{
  UINT8 D[7];
  if (!Time) { return EFI_INVALID_PARAMETER; }
  if (!mBus) { return EFI_UNSUPPORTED; }
  EFI_STATUS Status = Transfer (TRUE, D);
  if (EFI_ERROR (Status)) { return Status; }
  if (D[0] & BIT7) { return EFI_NOT_READY; } // oscillator stopped
  UINT8 HourMask = (D[2] & BIT6) ? 0x1f : 0x3f;
  UINT8 Fields[] = {D[0], D[1], D[2] & HourMask, D[4], D[5], D[6]};
  for (UINTN I = 0; I < ARRAY_SIZE (Fields); I++) {
    if ((Fields[I] & 15) > 9 || (Fields[I] >> 4) > 9) { return EFI_DEVICE_ERROR; }
  }
  ZeroMem (Time, sizeof (*Time));
  Time->Second = BcdToDecimal8 (D[0]);
  Time->Minute = BcdToDecimal8 (D[1]);
  Time->Hour = BcdToDecimal8 (D[2] & HourMask);
  if (D[2] & BIT6) {
    UINTN Hour = BcdToDecimal8 (D[2] & 0x1f);
    if (Hour < 1 || Hour > 12) { return EFI_DEVICE_ERROR; }
    Time->Hour = Hour % 12 + ((D[2] & BIT5) ? 12 : 0);
  }
  Time->Day = BcdToDecimal8 (D[4]);
  Time->Month = BcdToDecimal8 (D[5]);
  Time->Year = 2000 + BcdToDecimal8 (D[6]);
  Time->TimeZone = EFI_UNSPECIFIED_TIMEZONE;
  if (!IsTimeValid (Time)) { return EFI_DEVICE_ERROR; }
  if (Caps) { Caps->Resolution = 1; Caps->Accuracy = 0; Caps->SetsToZero = TRUE; }
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
LibSetTime (EFI_TIME *Time)
{
  if (!Time || !IsTimeValid (Time) || Time->Year < 2000 || Time->Year > 2099) { return EFI_INVALID_PARAMETER; }
  if (!mBus) { return EFI_UNSUPPORTED; }
  UINT8 D[7] = {DecimalToBcd8 (Time->Second), DecimalToBcd8 (Time->Minute),
                DecimalToBcd8 (Time->Hour), (UINT8)((EfiTimeToEpoch (Time) / 86400 + 4) % 7 + 1),
                DecimalToBcd8 (Time->Day), DecimalToBcd8 (Time->Month), DecimalToBcd8 (Time->Year - 2000)};
  return Transfer (FALSE, D);
}

EFI_STATUS EFIAPI LibGetWakeupTime (BOOLEAN *Enabled, BOOLEAN *Pending, EFI_TIME *Time) { return EFI_UNSUPPORTED; }
EFI_STATUS EFIAPI LibSetWakeupTime (BOOLEAN Enabled, EFI_TIME *Time) { return EFI_UNSUPPORTED; }

STATIC VOID EFIAPI
VirtualAddressChange (EFI_EVENT Event, VOID *Context)
{
  EFI_STATUS Status;

  Status = EfiConvertPointer (0, (VOID **)&mBus);
  if (!EFI_ERROR (Status)) {
    Status = EfiConvertPointer (0, (VOID **)&mClockGate);
  }
  mRuntimeOwner = !EFI_ERROR (Status);
  mVirtual = TRUE;
}

STATIC EFI_STATUS
Configure (VOID *Fdt)
{
  INT32 Len, Rtc = FdtNodeOffsetByCompatible (Fdt, -1, "dallas,ds1307");
  if (Rtc < 0 || !FdtNodeEnabled (Fdt, Rtc)) { return EFI_NOT_FOUND; }
  CONST UINT32 *Reg = FdtGetProp (Fdt, Rtc, "reg", &Len);
  if (!Reg || Len != 4 || Fdt32ToCpu (*Reg) > 0x7f) { return EFI_COMPROMISED_DATA; }
  mAddress = Fdt32ToCpu (*Reg);
  INT32 Bus = FdtParentOffset (Fdt, Rtc);
  if (!FdtNodeEnabled (Fdt, Bus) || FdtStringListSearch (Fdt, Bus, "compatible", "snps,designware-i2c") < 0) {
    return EFI_UNSUPPORTED;
  }
  UINTN Base, Size;
  EFI_STATUS Status = FdtGetMmio (Fdt, Bus, 0, 0x100, &Base, &Size);
  if (!EFI_ERROR (Status)) { Status = FdtMapMmio (Base, Size); }
  if (EFI_ERROR (Status)) { return Status; }
  INT32 Clock = FdtNodeOffsetByCompatible (Fdt, -1, "sophgo,sg2042-clkgen");
  UINTN Gate;
  Status = FdtGetMmio (Fdt, Clock, 0, EFI_PAGE_SIZE, &Gate, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  if (((Base | Gate) & EFI_PAGE_MASK) || Size != EFI_PAGE_SIZE) { return EFI_UNSUPPORTED; }
  Status = FdtMapMmio (Gate, EFI_PAGE_SIZE);
  if (EFI_ERROR (Status)) { return Status; }
  mBus = Base;
  mClockGate = Gate;

  // Reserving an adapter also removes its other clients from Linux.
  INT32 Child;
  FdtForEachSubnode (Child, Fdt, Bus) {
    if (Child != Rtc && FdtNodeEnabled (Fdt, Child)) {
      return EFI_SUCCESS;
    }
  }

  Status = gDS->SetMemorySpaceAttributes (Gate, EFI_PAGE_SIZE, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gDS->SetMemorySpaceAttributes (Base, EFI_PAGE_SIZE, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
  if (EFI_ERROR (Status)) { return Status; }

  EFI_EVENT Notify;
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, VirtualAddressChange, NULL,
                               &gEfiEventVirtualAddressChangeGuid, &Notify);
  if (EFI_ERROR (Status)) { return Status; }
  if (FdtSetPropString (Fdt, Bus, "status", "reserved") != 0) {
    gBS->CloseEvent (Notify);
    return EFI_OUT_OF_RESOURCES;
  }
  mRuntimeOwner = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
LibRtcInitialize (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE               *Hob   = GetFirstGuidHob (&gFdtHobGuid);
  EFI_STATUS                      Status;
  EFI_TIME                        Time;
  EFI_RT_PROPERTIES_TABLE         *Properties;
  STATIC EFI_RT_PROPERTIES_TABLE  Defaults = {
    EFI_RT_PROPERTIES_TABLE_VERSION, sizeof (EFI_RT_PROPERTIES_TABLE), EFI_RT_SUPPORTED_GET_VARIABLE | EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME |
    EFI_RT_SUPPORTED_SET_VARIABLE | EFI_RT_SUPPORTED_SET_VIRTUAL_ADDRESS_MAP |
    EFI_RT_SUPPORTED_CONVERT_POINTER | EFI_RT_SUPPORTED_GET_NEXT_HIGH_MONOTONIC_COUNT |
    EFI_RT_SUPPORTED_RESET_SYSTEM | EFI_RT_SUPPORTED_UPDATE_CAPSULE |
    EFI_RT_SUPPORTED_QUERY_CAPSULE_CAPABILITIES | EFI_RT_SUPPORTED_QUERY_VARIABLE_INFO
  };

  if (EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiRtPropertiesTableGuid, (VOID **)&Properties))) {
    Properties = &Defaults;
    Status     = gBS->InstallConfigurationTable (&gEfiRtPropertiesTableGuid, Properties);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = EFI_NOT_FOUND;
  if (Hob && (GET_GUID_HOB_DATA_SIZE (Hob) >= sizeof (UINT64))) {
    VOID  *Fdt = (VOID *)(UINTN)ReadUnaligned64 (GET_GUID_HOB_DATA (Hob));
    if (Fdt && !FdtCheckHeader (Fdt)) {
      Status = Configure (Fdt);
    }
  }

  if (!EFI_ERROR (Status)) {
    Status = LibGetTime (&Time, NULL);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DS1307 RTC read: %r\n", Status));
  } else {
    DEBUG ((DEBUG_INFO, "DS1307 RTC: %04u-%02u-%02u %02u:%02u:%02u\n", Time.Year, Time.Month, Time.Day, Time.Hour, Time.Minute, Time.Second));
  }

  Properties->RuntimeServicesSupported &= ~(EFI_RT_SUPPORTED_GET_TIME | EFI_RT_SUPPORTED_SET_TIME |
                                            EFI_RT_SUPPORTED_GET_WAKEUP_TIME | EFI_RT_SUPPORTED_SET_WAKEUP_TIME);
  if (mRuntimeOwner) {
    Properties->RuntimeServicesSupported |= EFI_RT_SUPPORTED_GET_TIME | EFI_RT_SUPPORTED_SET_TIME;
  }
  // Publish the architecture protocol even if the RTC is unavailable.
  return EFI_SUCCESS;
}
