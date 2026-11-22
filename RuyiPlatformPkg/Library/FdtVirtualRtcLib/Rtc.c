/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Guid/FdtHob.h>
#include <Guid/Fdt.h>
#include <Guid/RtPropertiesTable.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/FdtRtcSourceLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimeBaseLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Library/RealTimeClockLib.h>

STATIC EFI_STATUS
SetTimeCapabilities (VOID)
{
  EFI_RT_PROPERTIES_TABLE *Table;
  if (EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiRtPropertiesTableGuid, (VOID **)&Table))) {
    CONST EFI_RT_PROPERTIES_TABLE Initial = {
      EFI_RT_PROPERTIES_TABLE_VERSION, sizeof (EFI_RT_PROPERTIES_TABLE),
      EFI_RT_SUPPORTED_GET_TIME | EFI_RT_SUPPORTED_SET_TIME |
      EFI_RT_SUPPORTED_GET_VARIABLE | EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME |
      EFI_RT_SUPPORTED_SET_VARIABLE | EFI_RT_SUPPORTED_SET_VIRTUAL_ADDRESS_MAP |
      EFI_RT_SUPPORTED_CONVERT_POINTER | EFI_RT_SUPPORTED_GET_NEXT_HIGH_MONOTONIC_COUNT |
      EFI_RT_SUPPORTED_RESET_SYSTEM | EFI_RT_SUPPORTED_UPDATE_CAPSULE |
      EFI_RT_SUPPORTED_QUERY_CAPSULE_CAPABILITIES | EFI_RT_SUPPORTED_QUERY_VARIABLE_INFO
    };
    Table = AllocateRuntimeCopyPool (sizeof (Initial), &Initial);
    if (Table == NULL) { return EFI_OUT_OF_RESOURCES; }
    EFI_STATUS Status = gBS->InstallConfigurationTable (&gEfiRtPropertiesTableGuid, Table);
    if (EFI_ERROR (Status)) { FreePool (Table); }
    return Status;
  }
  Table->RuntimeServicesSupported &= ~(EFI_RT_SUPPORTED_GET_WAKEUP_TIME | EFI_RT_SUPPORTED_SET_WAKEUP_TIME);
  Table->RuntimeServicesSupported |= EFI_RT_SUPPORTED_GET_TIME | EFI_RT_SUPPORTED_SET_TIME;
  return EFI_SUCCESS;
}

#define NS_PER_SECOND  1000000000U

STATIC UINT16  mEpochYear;
STATIC INT64   mEpochSeconds;  // UTC seconds relative to January 1 of mEpochYear.
STATIC UINT32  mNanosecond;
STATIC UINT64  mCounter;
STATIC UINT64  mFrequency;
STATIC UINT64  mCounterStart;
STATIC UINT64  mCounterEnd;
STATIC EFI_TIME mSavedTime;

STATIC UINT64
ElapsedTicks (VOID)
{
  UINT64 Now = GetPerformanceCounter ();
  if (mCounterStart < mCounterEnd) {
    return Now >= mCounter ? Now - mCounter : (mCounterEnd - mCounter) + 1 + (Now - mCounterStart);
  }
  return Now <= mCounter ? mCounter - Now : (mCounter - mCounterEnd) + 1 + (mCounterStart - Now);
}

STATIC INT32
LocalOffset (CONST EFI_TIME *Time)
{
  // EFI_TIME.TimeZone already includes the daylight adjustment.
  return (Time->TimeZone == EFI_UNSPECIFIED_TIMEZONE) ? 0 : -Time->TimeZone * (INT32)SEC_PER_MIN;
}

STATIC VOID
StartClock (CONST EFI_TIME *Time)
{
  EFI_TIME Calendar = *Time;
  // Use a representable leap or common year in both conversion directions.
  Calendar.Year = IsLeapYear (&Calendar) ? 1972 : 1970;
  EFI_TIME January = { .Year = Calendar.Year, .Month = 1, .Day = 1 };
  UINTN Seconds = EfiTimeToEpoch (&Calendar) - EfiTimeToEpoch (&January);
  mEpochYear = Time->Year;
  mEpochSeconds = (INT64)Seconds - LocalOffset (Time);
  mNanosecond = Time->Nanosecond;
  mCounter = GetPerformanceCounter ();
}

EFI_STATUS EFIAPI
LibGetTime (EFI_TIME *Time, EFI_TIME_CAPABILITIES *Caps)
{
  if (Time == NULL) { return EFI_INVALID_PARAMETER; }
  // Keep the driver's legacy settings until the first time saved by this library.
  if (mSavedTime.Year != 0) {
    Time->TimeZone = mSavedTime.TimeZone;
    Time->Daylight = mSavedTime.Daylight;
  }

  UINT64 Remainder;
  UINT64 Elapsed = DivU64x64Remainder (ElapsedTicks (), mFrequency, &Remainder);
  if (Elapsed > (UINT64)(10000 - mEpochYear) * 366 * SEC_PER_DAY) { return EFI_DEVICE_ERROR; }
  UINT64 Fraction = mNanosecond + GetTimeInNanoSecond (Remainder);
  INT64 Seconds = mEpochSeconds + (INT64)Elapsed + LocalOffset (Time) + (INT64)(Fraction / NS_PER_SECOND);

  EFI_TIME Year = { .Year = mEpochYear };
  if (Seconds < 0) {
    if (Year.Year == 1900) { return EFI_DEVICE_ERROR; }
    Year.Year--;
    Seconds += (IsLeapYear (&Year) ? 366 : 365) * SEC_PER_DAY;
  }
  for (;;) {
    UINT32 YearSeconds = (IsLeapYear (&Year) ? 366 : 365) * SEC_PER_DAY;
    if (Seconds < YearSeconds) { break; }
    if (Year.Year == 9999) { return EFI_DEVICE_ERROR; }
    Seconds -= YearSeconds;
    Year.Year++;
  }

  // Convert the day within the year using 1970 or 1972 as the matching
  // common or leap year. The input fits TimeBaseLib's UINTN on 32-bit targets.
  EpochToEfiTime ((UINTN)Seconds + (IsLeapYear (&Year) ? 730U * SEC_PER_DAY : 0), Time);
  Time->Year = Year.Year;
  Time->Nanosecond = (UINT32)(Fraction % NS_PER_SECOND);
  if (!IsTimeValid (Time)) { return EFI_DEVICE_ERROR; }
  if (Caps != NULL) {
    Caps->Resolution = (UINT32)MIN (mFrequency, NS_PER_SECOND);
    Caps->Accuracy = 0;
    Caps->SetsToZero = FALSE;
  }
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
LibSetTime (EFI_TIME *Time)
{
  if (Time == NULL || !IsTimeValid (Time)) { return EFI_INVALID_PARAMETER; }
  EFI_STATUS Status = EfiSetVariable (
                        (CHAR16 *)FixedPcdGetPtr (PcdVirtualRtcTimeVariableName), &gEfiCallerIdGuid,
                        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                        sizeof (*Time), Time
                        );
  if (EFI_ERROR (Status)) { return EFI_DEVICE_ERROR; }
  StartClock (Time);
  mSavedTime = *Time;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
LibGetWakeupTime (BOOLEAN *Enabled, BOOLEAN *Pending, EFI_TIME *Time)
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS EFIAPI
LibSetWakeupTime (BOOLEAN Enabled, EFI_TIME *Time)
{
  return EFI_UNSUPPORTED;
}

STATIC EFI_STATUS
ReadBootTime (EFI_TIME *Time)
{
  CONST VOID *Fdt;
  VOID *Hob = GetFirstGuidHob (&gFdtHobGuid);
  if (Hob != NULL) {
    if (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (UINT64)) { return EFI_COMPROMISED_DATA; }
    UINT64 Address = ReadUnaligned64 (GET_GUID_HOB_DATA (Hob));
    if (Address > MAX_UINTN) { return EFI_UNSUPPORTED; }
    Fdt = (CONST VOID *)(UINTN)Address;
  } else {
    VOID *Table;
    EFI_STATUS Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Table);
    if (EFI_ERROR (Status)) { return Status; }
    Fdt = Table;
  }
  if (Fdt == NULL || FdtCheckHeader (Fdt)) { return EFI_COMPROMISED_DATA; }
  return FdtRtcReadTime (Fdt, Time);
}

EFI_STATUS EFIAPI
LibRtcInitialize (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  mFrequency = GetPerformanceCounterProperties (&mCounterStart, &mCounterEnd);
  if (!mFrequency || mCounterStart == mCounterEnd) { return EFI_DEVICE_ERROR; }
  EFI_STATUS Status = SetTimeCapabilities ();
  if (EFI_ERROR (Status)) { return Status; }

  UINTN Size = sizeof (mSavedTime);
  Status = EfiGetVariable ((CHAR16 *)FixedPcdGetPtr (PcdVirtualRtcTimeVariableName),
                           &gEfiCallerIdGuid, NULL, &Size, &mSavedTime);
  if (EFI_ERROR (Status) || Size != sizeof (mSavedTime) || !IsTimeValid (&mSavedTime)) {
    mSavedTime.Year = 0;
  }

  EFI_TIME Time = { .TimeZone = EFI_UNSPECIFIED_TIMEZONE };
  Status = ReadBootTime (&Time);
  if (!EFI_ERROR (Status) && !IsTimeValid (&Time)) { Status = EFI_DEVICE_ERROR; }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "No valid boot RTC sample: %r\n", Status));
    if (mSavedTime.Year != 0) {
      Time = mSavedTime;
    } else {
      Time = (EFI_TIME){ .TimeZone = EFI_UNSPECIFIED_TIMEZONE };
      EpochToEfiTime (BUILD_EPOCH, &Time);
    }
  }
  StartClock (&Time);
  return EFI_SUCCESS;
}
