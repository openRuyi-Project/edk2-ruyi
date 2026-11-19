/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/CpuMmioAddressLib.h>
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/FdtBoardInfoLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/Sg2042ClockLib.h>
#include <Library/TimerLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/SdMmcOverride.h>

#define SDHCI_CLOCK_CONTROL      0x2c
#define SDHCI_CLOCK_CARD_ENABLE  BIT2
#define SD_DEFAULT_SPEED_MHZ     25

typedef struct {
  EFI_HANDLE Handle;
  UINTN Base;
  UINT32 BaseClockMHz;
  UINT8 BusWidth;
  BOOLEAN Embedded;
  EDKII_SD_MMC_SLOT_CONFIG Slot;
} SG_MMC_PORT;

STATIC SG_MMC_PORT mPorts[2];
STATIC UINTN mCount;

STATIC SG_MMC_PORT *
GetPort (EFI_HANDLE Handle, UINT8 Slot)
{
  for (UINTN I = 0; I < mCount; I++) {
    if (Slot == 0 && mPorts[I].Handle == Handle) { return &mPorts[I]; }
  }
  return NULL;
}

STATIC EFI_STATUS EFIAPI
Capability (EFI_HANDLE Handle, UINT8 Slot, VOID *Caps, UINT32 *Clock)
{
  SG_MMC_PORT *Port = GetPort (Handle, Slot);
  if (!Port) { return EFI_NOT_FOUND; }
  if (!Caps || !Clock) { return EFI_INVALID_PARAMETER; }
  UINT64 Value = ReadUnaligned64 (Caps);
  // Use PIO for the noncoherent on-chip hosts. PCIe endpoints use PCI DMA.
  Value &= ~(UINT64)(BIT19 | BIT22 | BIT18 | BIT26 | BIT25);
  Value &= ~(BIT32 | BIT33 | BIT34 | BIT63);
  if (Port->BusWidth == 8) { Value |= BIT18; }
  if (Port->Embedded) { Value = (Value & ~((UINT64)3 << 30)) | BIT30; }
  WriteUnaligned64 (Caps, Value);
  // The clock generator supplies the input used by the SDHCI divisor.
  *Clock = Port->BaseClockMHz;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
Notify (EFI_HANDLE Handle, UINT8 Slot, EDKII_SD_MMC_PHASE_TYPE Phase, VOID *Data)
{
  SG_MMC_PORT *Port = GetPort (Handle, Slot);
  if (!Port) { return EFI_NOT_FOUND; }
  if (Phase == EdkiiSdMmcGetSlotConfig) {
    if (!Data) { return EFI_INVALID_PARAMETER; }
    *(EDKII_SD_MMC_SLOT_CONFIG *)Data = Port->Slot;
  } else if (Phase == EdkiiSdMmcGetOperatingParam) {
    if (!Data) { return EFI_INVALID_PARAMETER; }
    EDKII_SD_MMC_OPERATING_PARAMETERS *Params = Data;
    Params->BusWidth = Port->BusWidth;
    if (!Port->Embedded && Port->Slot.NoMmc) {
      Params->ClockFreq = MIN (Params->ClockFreq, SD_DEFAULT_SPEED_MHZ);
    }
  } else if (Phase == EdkiiSdMmcInitHostPost) {
    // Power cycling the slot clears SDCLK enable on SG2042. Supply the
    // startup clocks with VDD on before issuing the first command.
    MmioOr16 (CpuMmioAddress (Port->Base) + SDHCI_CLOCK_CONTROL, SDHCI_CLOCK_CARD_ENABLE);
    MicroSecondDelay (1000);
  } else if (Phase == EdkiiSdMmcResetPost) {
    // SG2042 DWC MSHC PHY, single-ended 3.3 V pads. The PHY is reset
    // by a full SDHCI reset, including resets issued during enumeration.
    UINTN B = CpuMmioAddress (Port->Base);
    UINT16 Pad = 2 | (3 << 5) | (2 << 9);
    MmioAndThenOr32 (B + 0x300, ~(UINT32)(BIT0 | (0xffU << 16)), BIT1 | (0x89U << 16));
    MmioWrite16 (B + 0x304, Pad | BIT3);
    MmioWrite16 (B + 0x306, Pad | BIT3);
    MmioWrite16 (B + 0x308, Pad);
    MmioWrite16 (B + 0x30a, Pad | BIT4);
    MmioWrite16 (B + 0x30c, Pad | BIT3);
    MmioWrite8 (B + 0x31d, BIT0 | BIT4);
    MmioWrite8 (B + 0x31e, 10);
    MmioWrite8 (B + 0x31d, BIT0);
    MmioWrite8 (B + 0x320, BIT1);
    MmioWrite8 (B + 0x321, 2 << 2);
    MmioOr32 (B + 0x300, BIT0);
    if (Port->Embedded) { MmioOr8 (B + 0x28, BIT6 | BIT7); }
  }
  return EFI_SUCCESS;
}

STATIC EDKII_SD_MMC_OVERRIDE mOverride = {
  EDKII_SD_MMC_OVERRIDE_PROTOCOL_VERSION, Capability, Notify, NULL
};

EFI_STATUS EFIAPI
Sg2042MmcEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  CONST VOID *Fdt;
  RUYI_BOARD_INFO Info;
  EFI_HANDLE Override = NULL;
  INT32 Node = -1, Length;
  UINTN Gates, Size;
  EFI_STATUS Status = FdtBoardInfoGet (&Info, &Fdt);
  if (EFI_ERROR (Status)) { return Status; }
  INT32 Cg = FdtNodeOffsetByCompatible (Fdt, -1, "sophgo,sg2042-clkgen");
  Status = FdtGetMmio (Fdt, Cg, 0, 8, &Gates, &Size);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (Gates, Size);
  if (EFI_ERROR (Status)) { return Status; }
  Gates = CpuMmioAddress (Gates);
  Status = gBS->InstallProtocolInterface (&Override, &gEdkiiSdMmcOverrideProtocolGuid, EFI_NATIVE_INTERFACE, &mOverride);
  if (EFI_ERROR (Status)) { return Status; }
  while ((Node = FdtNodeOffsetByCompatible (Fdt, Node, "sophgo,sg2042-dwcmshc")) >= 0) {
    if (!FdtNodeEnabled (Fdt, Node)) { continue; }
    if (mCount == ARRAY_SIZE (mPorts)) { break; }
    SG_MMC_PORT *Port = &mPorts[mCount];
    CONST UINT32 *Width = FdtGetProp (Fdt, Node, "bus-width", &Length);
    if (!Width || Length != 4) { continue; }
    UINT32 Bits = Fdt32ToCpu (*Width);
    if (Bits != 1 && Bits != 4 && Bits != 8) { continue; }
    Port->BusWidth = (UINT8)Bits;
    Port->Embedded = FdtGetProp (Fdt, Node, "non-removable", NULL) != NULL;
    UINT64 Rate = Sg2042GetClockRate (Fdt, Port->Embedded ? Sg2042EmmcClock : Sg2042SdClock);
    if (Rate == 0) { continue; }
    Port->BaseClockMHz = (UINT32)((Rate + 999999) / 1000000);
    Port->Slot.NoMmc = FdtGetProp (Fdt, Node, "no-mmc", NULL) != NULL;
    Port->Slot.NoSd = FdtGetProp (Fdt, Node, "no-sd", NULL) != NULL;
    Port->Slot.NoSdio = FdtGetProp (Fdt, Node, "no-sdio", NULL) != NULL;
    // These boards have no 1.2 V or switched 1.8 V HS200/HS400 supply.
    Port->Slot.ExcludedEmmcDeviceTypes = 0xfc;
    Status = FdtGetMmio (Fdt, Node, 0, EFI_PAGE_SIZE, &Port->Base, &Size);
    if (EFI_ERROR (Status)) { continue; }
    Status = FdtMapMmio (Port->Base, Size);
    if (EFI_ERROR (Status)) { continue; }
    MmioOr32 (Gates + 4, Port->Embedded ? (BIT2 | BIT3 | BIT4) : (BIT5 | BIT6 | BIT7));
    Status = RegisterNonDiscoverableMmioDevice (NonDiscoverableDeviceTypeSdhci,
              NonDiscoverableDeviceDmaTypeNonCoherent, NULL, &Port->Handle, 1, Port->Base, Size);
    if (!EFI_ERROR (Status)) { mCount++; }
  }
  if (mCount) { return EFI_SUCCESS; }
  gBS->UninstallProtocolInterface (Override, &gEdkiiSdMmcOverrideProtocolGuid, &mOverride);
  return EFI_NOT_FOUND;
}
