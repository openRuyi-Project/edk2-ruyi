/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Host.h"
#include <Guid/EventGroup.h>

EIC_MMC_PORT mPorts[2];
UINTN mPortCount;
STATIC EFI_EVENT mDetectEvent;

EIC_MMC_PORT *
PortFor (EFI_HANDLE Handle, UINT8 Slot)
{
  UINTN I;
  if (Slot != 0) { return NULL; }
  for (I = 0; I < mPortCount; I++) {
    if (mPorts[I].Handle == Handle) { return &mPorts[I]; }
  }
  return NULL;
}

STATIC EFI_STATUS EFIAPI
Capabilities (EFI_HANDLE Handle, UINT8 Slot, VOID *Capability, UINT32 *BaseClock)
{
  EIC_MMC_PORT *Port = PortFor (Handle, Slot);
  UINT64 Value;
  if (!Port) { return EFI_NOT_FOUND; }
  if (!Capability || !BaseClock) { return EFI_INVALID_PARAMETER; }
  Value = ReadUnaligned64 (Capability);
  // The eMMC uses 3.3 V VCC and fixed 1.8 V signaling.
  // Signal voltage is selected separately from the card power supply.
  Value &= ~(BIT30 | BIT31 | (0xffULL << 48));
  if (Port->Emmc) {
    Value |= BIT30 | BIT18 | BIT24;
    Value &= ~(BIT25 | BIT26);
    if (Port->Hs400) { Value |= BIT63; }
    else { Value &= ~BIT63; }
  } else {
    Value &= ~(BIT18 | BIT63);
  }
  WriteUnaligned64 (Capability, Value);
  *BaseClock = Port->MaxClock / 1000000;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
Notify (EFI_HANDLE Handle, UINT8 Slot, EDKII_SD_MMC_PHASE_TYPE Phase, VOID *Data)
{
  EIC_MMC_PORT *Port = PortFor (Handle, Slot);
  EDKII_SD_MMC_OPERATING_PARAMETERS *Parameters;
  SD_MMC_BUS_MODE Mode;
  UINT16 Control;
  UINT32 Divider, Hertz;
  EFI_STATUS Status;

  if (!Port) { return EFI_NOT_FOUND; }
  switch (Phase) {
    case EdkiiSdMmcGetOperatingParam:
      if (!Data) { return EFI_INVALID_PARAMETER; }
      Parameters = Data;
      Parameters->BusWidth = Port->Width;
      Parameters->ClockFreq = Port->MaxClock / 1000000;
      Parameters->DriverStrength.Sd = SdDriverStrengthIgnore;
      return EFI_SUCCESS;
    case EdkiiSdMmcGetDmaBoundary:
      if (!Data) { return EFI_INVALID_PARAMETER; }
      *(UINT64 *)Data = EIC_DMA_BOUNDARY;
      return EFI_SUCCESS;
    case EdkiiSdMmcResetPre:
    case EdkiiSdMmcInitHostPre:
      return SetCardClock (Port, 400000);
    case EdkiiSdMmcResetPost:
      return ConfigurePhy (Port);
    case EdkiiSdMmcInitHostPost:
      Status = SetCardClock (Port, 400000);
      if (EFI_ERROR (Status)) { return Status; }
      if (Port->Emmc) { MmioOr16 (Port->Base + HC_HOST_CTRL2, BIT3); }
      // This host accepts CARD_EN only after bus power is enabled.
      MmioOr16 (Port->Base + HC_CLOCK_CTRL, HC_CARD_CLOCK);
      return EFI_SUCCESS;
    case EdkiiSdMmcUhsSignaling:
      if (!Data) { return EFI_INVALID_PARAMETER; }
      Mode = *(SD_MMC_BUS_MODE *)Data;
      if (Port->Emmc) { MmioOr16 (Port->Base + HC_HOST_CTRL2, BIT3); }
      if (Port->Emmc && Mode == SdMmcMmcHs400) {
        MmioOr8 (Port->Base + HC_HOST_CTRL, BIT2);
        MmioAndThenOr16 (Port->Base + HC_HOST_CTRL2, (UINT16)~7U, 7);
      }
      return EFI_SUCCESS;
    case EdkiiSdMmcSwitchClockFreqPost:
      if (!Data) { return EFI_INVALID_PARAMETER; }
      Mode = *(SD_MMC_BUS_MODE *)Data;
      Control = MmioRead16 (Port->Base + HC_CLOCK_CTRL);
      Divider = ((Control >> 8) & 0xff) | ((Control & 0xc0) << 2);
      Hertz = Divider ? Port->MaxClock / (2 * Divider) : Port->MaxClock;
      Status = SetCardClock (Port, Hertz);
      if (!EFI_ERROR (Status) && Port->Emmc && Mode == SdMmcMmcHs400) {
        Status = SetDll (Port);
      }
      return Status;
    default:
      return EFI_UNSUPPORTED;
  }
}

STATIC EDKII_SD_MMC_OVERRIDE mOverride = {
  EDKII_SD_MMC_OVERRIDE_PROTOCOL_VERSION, Capabilities, Notify, ExecuteTuning
};

STATIC VOID EFIAPI
Detect (EFI_EVENT Event, VOID *Context)
{
  UINTN I;
  for (I = 0; I < mPortCount; I++) {
    if (mPorts[I].Ready) { RefreshCardDetect (&mPorts[I]); }
  }
}

STATIC VOID EFIAPI
ExitBoot (EFI_EVENT Event, VOID *Context)
{
  if (mDetectEvent) {
    gBS->CloseEvent (mDetectEvent);
    mDetectEvent = NULL;
  }
}

EFI_STATUS EFIAPI
Eic7700SdhciEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  RUYI_BOARD_INFO Info;
  CONST VOID *Fdt;
  INT32 Node = -1;
  EFI_STATUS Status, Result = EFI_NOT_FOUND;
  EFI_EVENT ExitEvent;
  EFI_HANDLE OverrideHandle = NULL;
  EIC_MMC_PORT *Port;

  Status = FdtBoardInfoGet (&Info, &Fdt);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Detect, NULL, &mDetectEvent);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ExitBoot, NULL, &gEfiEventExitBootServicesGuid, &ExitEvent);
  if (EFI_ERROR (Status)) { gBS->CloseEvent (mDetectEvent); return Status; }
  Status = gBS->SetTimer (mDetectEvent, TimerPeriodic, 100 * 10000ULL);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mDetectEvent); gBS->CloseEvent (ExitEvent); return Status;
  }
  Status = gBS->InstallProtocolInterface (&OverrideHandle, &gEdkiiSdMmcOverrideProtocolGuid,
                                         EFI_NATIVE_INTERFACE, &mOverride);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mDetectEvent); gBS->CloseEvent (ExitEvent); return Status;
  }
  while ((Node = FdtNodeOffsetByCompatible (Fdt, Node, "eswin,eic7700-dwcmshc")) >= 0) {
    if (!FdtNodeEnabled (Fdt, Node)) { continue; }
    if (mPortCount == ARRAY_SIZE (mPorts)) { Result = EFI_UNSUPPORTED; break; }
    Port = &mPorts[mPortCount];
    ZeroMem (Port, sizeof (*Port));
    Status = ParsePort (Fdt, Node, Port);
    if (!EFI_ERROR (Status)) { Status = InitializePort (Port); }
    if (!EFI_ERROR (Status)) {
      Status = RegisterNonDiscoverableMmioDevice (NonDiscoverableDeviceTypeSdhci,
                 NonDiscoverableDeviceDmaTypeNonCoherent, NULL, &Port->Handle,
                 1, Port->Base, (UINTN)EIC_MMC_REGISTER_SIZE);
    }
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "EIC7700 MMC node %d: %r\n", Node, Status));
      if (Result != EFI_SUCCESS) { Result = Status; }
      continue;
    }
    Port->Ready = TRUE;
    mPortCount++;
    Result = EFI_SUCCESS;
    DEBUG ((DEBUG_INFO, "EIC7700 %a controller at %Lx\n", Port->Emmc ? "eMMC" : "SD", (UINT64)Port->Base));
  }
  if (!mPortCount) {
    gBS->UninstallProtocolInterface (OverrideHandle, &gEdkiiSdMmcOverrideProtocolGuid, &mOverride);
    gBS->CloseEvent (mDetectEvent); gBS->CloseEvent (ExitEvent);
    return Result;
  }
  return EFI_SUCCESS;
}
