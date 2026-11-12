/** @file
  Establish Gen1 before a DesignWare directed speed change.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <IndustryStandard/DwcPcieRegisters.h>
#include <IndustryStandard/Pci.h>
#include <Library/DebugLib.h>
#include <Library/DwcPcieTrainingLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#define DWC_SPEED_CONTROL       0x80c
#define DWC_SPEED_CHANGE        BIT17
#define PCIE_LINK_CAP           0x0c
#define PCIE_LINK_STATUS        0x12
#define PCIE_LINK_CAP2          0x2c
#define PCIE_LINK_CONTROL2      0x30
#define PCIE_SPEED_MASK         0x0fU
#define PCIE_WIDTH_MASK         0x3fU
#define PCIE_LINK_TRAINING      BIT11
#define PCIE_SPEED_VECTOR       0xfeU
#define POLL_INTERVAL_US        1000
#define LINK_TIMEOUT_US         1000000
#define CONFIG_DELAY_US         100000

STATIC BOOLEAN
ValidWidth (
  UINT8  Width
  )
{
  return Width == 1 || Width == 2 || Width == 4 || Width == 8 ||
         Width == 12 || Width == 16 || Width == 32;
}

STATIC EFI_STATUS
ReadConfig (
  CONST DWC_PCIE_TRAINING  *Training,
  BOOLEAN                  Partner,
  UINT16                   Register,
  UINT32                   *Value
  )
{
  if (!Partner) {
    *Value = MmioRead32 (Training->Dbi + Register);
    return EFI_SUCCESS;
  }

  if (!Training->LinkReady (Training->Context)) {
    return EFI_NOT_READY;
  }

  return Training->ReadPartner (Training->Context, Register, Value);
}

STATIC EFI_STATUS
FindPcieCapability (
  CONST DWC_PCIE_TRAINING  *Training,
  BOOLEAN                  Partner,
  UINT8                    *Capability,
  UINT8                    *Version
  )
{
  EFI_STATUS  Status;
  UINT32      Value;
  UINT8       Offset;

  Status = ReadConfig (Training, Partner, PCI_CAPBILITY_POINTER_OFFSET, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Offset = (UINT8)Value;
  for (UINTN Index = 0; Index < 48; Index++) {
    if (Offset == 0) {
      return EFI_UNSUPPORTED;
    }

    if (Offset < 0x40 || (Offset & 3) != 0) {
      return EFI_COMPROMISED_DATA;
    }

    Status = ReadConfig (Training, Partner, Offset, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((UINT8)Value == EFI_PCI_CAPABILITY_ID_PCIEXP) {
      *Version = (Value >> 16) & 0xf;
      if (*Version == 0 || Offset > (*Version >= 2 ? 0xcc : 0xe8)) {
        return EFI_COMPROMISED_DATA;
      }

      *Capability = Offset;
      return EFI_SUCCESS;
    }

    Offset = (UINT8)(Value >> 8);
  }

  return EFI_COMPROMISED_DATA;
}

STATIC VOID
SetSpeedCapability (
  CONST DWC_PCIE_TRAINING  *Training,
  UINT8                    Capability,
  UINT32                   Value
  )
{
  MmioOr32 (Training->Dbi + DWC_DBI_MISC_CONTROL, DWC_DBI_RO_WRITE_ENABLE);
  MmioWrite32 (Training->Dbi + Capability + PCIE_LINK_CAP, Value);
  MmioAnd32 (Training->Dbi + DWC_DBI_MISC_CONTROL, ~(UINT32)DWC_DBI_RO_WRITE_ENABLE);
}

STATIC EFI_STATUS
WaitLink (
  CONST DWC_PCIE_TRAINING  *Training,
  UINT8                    Capability,
  UINT8                    Speed,
  UINT8                    Width,
  UINT16                   *LinkStatus
  )
{
  UINTN   Stable = 0;
  UINT32  SpeedControl = DWC_SPEED_CHANGE;

  *LinkStatus = 0;
  for (UINTN Elapsed = 0; Elapsed <= LINK_TIMEOUT_US; Elapsed += POLL_INTERVAL_US) {
    // Some integrations lose DBI access on a failed link. Poll the safe
    // platform status first, including during Recovery and Equalization.
    if (Training->LinkReady (Training->Context)) {
      *LinkStatus = MmioRead16 (Training->Dbi + Capability + PCIE_LINK_STATUS);
      SpeedControl = MmioRead32 (Training->Dbi + DWC_SPEED_CONTROL);
      if (!(SpeedControl & DWC_SPEED_CHANGE) && !(*LinkStatus & PCIE_LINK_TRAINING) &&
          (*LinkStatus & PCIE_SPEED_MASK) == Speed &&
          ValidWidth ((*LinkStatus >> 4) & PCIE_WIDTH_MASK) &&
          (Width == 0 || ((*LinkStatus >> 4) & PCIE_WIDTH_MASK) == Width))
      {
        // Include the post-link delay before issuing configuration requests.
        if (Stable >= CONFIG_DELAY_US) {
          return EFI_SUCCESS;
        }

        Stable += POLL_INTERVAL_US;
      } else {
        Stable = 0;
      }
    } else {
      Stable = 0;
    }

    MicroSecondDelay (POLL_INTERVAL_US);
  }

  DEBUG ((DEBUG_ERROR, "%a PCIe: expected Gen%u x%u, last link-status=%04x speed-control=%08x\n",
          Training->Name, Speed, Width, *LinkStatus, SpeedControl));
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
PartnerLimits (
  CONST DWC_PCIE_TRAINING  *Training,
  UINT8                    LocalWidth,
  UINT8                    *Speed,
  UINT8                    *Width
  )
{
  EFI_STATUS  Status;
  UINT32      Value;
  UINT32      Speeds;
  UINT8       Capability, Version, Maximum;
  BOOLEAN     Ready = FALSE;

  // Allow Configuration Request Retry Status after reset. Unsupported
  // requests can read as all ones until the partner is ready.
  for (UINTN Elapsed = 0; Elapsed <= LINK_TIMEOUT_US; Elapsed += POLL_INTERVAL_US) {
    Status = ReadConfig (Training, TRUE, PCI_VENDOR_ID_OFFSET, &Value);
    if (!EFI_ERROR (Status) && Value != MAX_UINT32 &&
        (UINT16)Value != 0 && (UINT16)Value != 1 && (UINT16)Value != MAX_UINT16)
    {
      Ready = TRUE;
      break;
    }

    if (EFI_ERROR (Status) && Status != EFI_NOT_READY) {
      return Status;
    }

    MicroSecondDelay (POLL_INTERVAL_US);
  }

  if (!Ready) {
    return EFI_TIMEOUT;
  }

  Status = FindPcieCapability (Training, TRUE, &Capability, &Version);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = ReadConfig (Training, TRUE, Capability + PCIE_LINK_CAP, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Maximum = Value & PCIE_SPEED_MASK;
  *Width = (Value >> 4) & PCIE_WIDTH_MASK;
  if (Maximum == 0 || Maximum > 7 || !ValidWidth (*Width)) {
    return EFI_COMPROMISED_DATA;
  }

  Speeds = (1U << (Maximum + 1)) - 2;
  if (Version >= 2) {
    Status = ReadConfig (Training, TRUE, Capability + PCIE_LINK_CAP2, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Value == MAX_UINT32) {
      return EFI_DEVICE_ERROR;
    }

    if (Value & PCIE_SPEED_VECTOR) {
      Speeds &= Value & PCIE_SPEED_VECTOR;
    }
  }

  for (*Speed = Training->MaxSpeed; *Speed > 0; (*Speed)--) {
    if (Speeds & (1U << *Speed)) {
      *Width = MIN (*Width, LocalWidth);
      return EFI_SUCCESS;
    }
  }

  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
DwcPcieTrainLink (
  IN CONST DWC_PCIE_TRAINING  *Training,
  OUT UINT16                *LinkStatus
  )
{
  EFI_STATUS   Status;
  CONST CHAR8  *Phase = "initial Gen1 link";
  UINT32       LinkCap;
  UINT8        Capability, Version, Speed, Width, LocalWidth;

  if (Training == NULL || LinkStatus == NULL || Training->Name == NULL ||
      Training->LinkReady == NULL || Training->EnableLtssm == NULL ||
      Training->ReadPartner == NULL || Training->Dbi == 0 || (Training->Dbi & 3) != 0 ||
      Training->MaxSpeed == 0 ||
      Training->MaxSpeed > 3 || !ValidWidth (Training->MaxWidth))
  {
    return EFI_INVALID_PARAMETER;
  }

  *LinkStatus = 0;
  Status = FindPcieCapability (Training, FALSE, &Capability, &Version);
  if (EFI_ERROR (Status) || Version < 2) {
    return EFI_ERROR (Status) ? Status : EFI_UNSUPPORTED;
  }

  LinkCap = MmioRead32 (Training->Dbi + Capability + PCIE_LINK_CAP);
  LocalWidth = (LinkCap >> 4) & PCIE_WIDTH_MASK;
  if (!ValidWidth (LocalWidth)) {
    return EFI_COMPROMISED_DATA;
  }

  LocalWidth = MIN (LocalWidth, Training->MaxWidth);
  // Use the platform's hardware limit, not a stale temporary Gen1 value.
  LinkCap = (LinkCap & ~PCIE_SPEED_MASK) | Training->MaxSpeed;
  SetSpeedCapability (Training, Capability, (LinkCap & ~PCIE_SPEED_MASK) | 1);
  MmioAndThenOr16 (Training->Dbi + Capability + PCIE_LINK_CONTROL2,
                  (UINT16)~PCIE_SPEED_MASK, 1);
  MmioAnd32 (Training->Dbi + DWC_SPEED_CONTROL, ~(UINT32)DWC_SPEED_CHANGE);
  Training->EnableLtssm (Training->Context);
  Status = WaitLink (Training, Capability, 1, 0, LinkStatus);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  SetSpeedCapability (Training, Capability, LinkCap);
  Phase = "link partner capabilities";
  Status = PartnerLimits (Training, LocalWidth, &Speed, &Width);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  DEBUG ((DEBUG_INFO, "%a PCIe: Gen1 x%u established, target Gen%u x%u\n",
          Training->Name, (*LinkStatus >> 4) & PCIE_WIDTH_MASK, Speed, Width));
  if (Speed > 1) {
    // DWC requires TLS followed by a 0-to-1 Directed Speed Change edge.
    MmioAndThenOr16 (Training->Dbi + Capability + PCIE_LINK_CONTROL2,
                    (UINT16)~PCIE_SPEED_MASK, Speed);
    MmioAnd32 (Training->Dbi + DWC_SPEED_CONTROL, ~(UINT32)DWC_SPEED_CHANGE);
    MmioOr32 (Training->Dbi + DWC_SPEED_CONTROL, DWC_SPEED_CHANGE);
  }

  Phase = "directed speed change";
  Status = WaitLink (Training, Capability, Speed, Width, LinkStatus);
Done:
  if (EFI_ERROR (Status)) {
    // The caller resets the platform. Do not access DBI on this path.
    DEBUG ((DEBUG_ERROR, "%a PCIe: %a failed: %r\n", Training->Name, Phase, Status));
  }

  return Status;
}
