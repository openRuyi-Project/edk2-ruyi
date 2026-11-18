/** SPDX-License-Identifier: BSD-2-Clause-Patent */

/* Clause 22 transaction reference: BSD SpacemiT DwEqosDxeUtil.
 * Copyright (c) 2025, Spacemit Corporation. All rights reserved.
 * RTL8211F register/mode semantics checked against the pinned PHY sources.
 */
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/EqosLib.h>
#include <Library/TimerLib.h>

#include "Registers.h"

#define RTL8211F_ID             0x001cc916U
#define YT8531_ID               0x4f51e91bU
#define YT8531_EXT_INDEX        0x1e
#define YT8531_EXT_DATA         0x1f
#define YT8531_CLOCK_GATING     0x0c
#define YT8531_GATE_RXC_LINK_DOWN  BIT12
#define YT8531_SLEEP_CONTROL    0x27
#define YT8531_AUTO_SLEEP       BIT15
#define YT8531_CHIP_CONFIG      0xa001
#define YT8531_RGMII_CONFIG     0xa003
#define YT8531_DELAY_1950_PS    13U
#define YT8531_STATUS           0x11

STATIC EFI_STATUS
WaitIdle (
  EQOS_PORT  *P
  )
{
  for (UINTN Elapsed = 0; Elapsed <= 10000; Elapsed += 10) {
    if (!(MmioRead32 (P->Base + MAC_MDIO_ADDRESS) & MDIO_BUSY)) {
      return EFI_SUCCESS;
    }

    if (Elapsed < 10000) {
      MicroSecondDelay (10);
    }
  }

  P->Faulted = TRUE;
  P->Error   = EFI_TIMEOUT;
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
Transfer (
  EQOS_PORT  *P,
  UINT32        Register,
  UINT16        *Value,
  BOOLEAN       Write
  )
{
  if (!P || !Value || (Register > 31) || (P->PhyAddress > 31)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!P->Ready || P->Faulted) {
    return EFI_NOT_READY;
  }

  EFI_STATUS  Status = WaitIdle (P);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Write) {
    MmioWrite32 (P->Base + MAC_MDIO_DATA, *Value);
  }

  // Always construct a Clause 22 command; never inherit C45/preamble state.
  UINT32  Command = (P->PhyAddress << MDIO_PHY_SHIFT) | (Register << MDIO_REGISTER_SHIFT) | ((UINT32)P->MdioRange << MDIO_CLOCK_SHIFT) |
                    (Write ? MDIO_WRITE : MDIO_READ) | MDIO_BUSY;

  MmioWrite32 (P->Base + MAC_MDIO_ADDRESS, Command);
  Status = WaitIdle (P);
  if (!EFI_ERROR (Status) && !Write) {
    *Value = (UINT16)MmioRead32 (P->Base + MAC_MDIO_DATA);
  }

  return Status;
}

EFI_STATUS
EqosMdioRead (
  EQOS_PORT  *P,
  UINT32        Register,
  UINT16        *Value
  )
{
  if (Value) {
    *Value = 0;
  }

  return Transfer (P, Register, Value, FALSE);
}

EFI_STATUS
EqosMdioWrite (
  EQOS_PORT  *P,
  UINT32        Register,
  UINT16        Value
  )
{
  return Transfer (P, Register, &Value, TRUE);
}

STATIC EFI_STATUS
Modify (
  EQOS_PORT  *P,
  UINT32        Register,
  UINT16        Mask,
  UINT16        Set
  )
{
  UINT16      Value;
  EFI_STATUS  Status = EqosMdioRead (P, Register, &Value);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  UINT16  New = (Value & ~Mask) | (Set & Mask);

  return New == Value ? EFI_SUCCESS : EqosMdioWrite (P, Register, New);
}

STATIC EFI_STATUS
RestoreSelector (
  EQOS_PORT  *P,
  UINT32        Register,
  UINT16        Saved,
  EFI_STATUS    Original
  )
{
  // A timed-out transaction can still execute. Do not issue a new command
  // until idle is actually observed; preserve a separate restore error.
  if (P->Faulted) {
    P->RestoreError = EFI_NOT_READY;
    return Original;
  }

  EFI_STATUS  Status = EqosMdioWrite (P, Register, Saved);

  if (EFI_ERROR (Status)) {
    P->RestoreError = Status;
    P->Faulted      = TRUE;
    return EFI_ERROR (Original) ? Original : Status;
  }

  return Original;
}

STATIC EFI_STATUS
PagedModify (
  EQOS_PORT  *P,
  UINT16        Page,
  UINT32        Register,
  UINT16        Mask,
  UINT16        Set
  )
{
  UINT16      Saved;
  EFI_STATUS  Status = EqosMdioRead (P, 31, &Saved);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EqosMdioWrite (P, 31, Page);
  if (!EFI_ERROR (Status)) {
    Status = Modify (P, Register, Mask, Set);
  }

  return RestoreSelector (P, 31, Saved, Status);
}

STATIC EFI_STATUS
SoftReset (
  EQOS_PORT  *P
  )
{
  EFI_STATUS  Status = Modify (P, 0, BIT15, BIT15);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  UINT16  Value;

  for (UINTN Elapsed = 0; Elapsed <= 500000; Elapsed += 5000) {
    Status = EqosMdioRead (P, 0, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (!(Value & BIT15)) {
      return EFI_SUCCESS;
    }

    if (Elapsed < 500000) {
      MicroSecondDelay (5000);
    }
  }

  P->Faulted = TRUE;
  P->Error   = EFI_TIMEOUT;
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
ConfigureRtl8211f (EQOS_PORT *P)
{
  EFI_STATUS Status = SoftReset (P);
  if (!EFI_ERROR (Status)) {
    Status = PagedModify (P, 0xd08, 0x11, BIT8, (P->DelayFlags & 1) ? BIT8 : 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = PagedModify (P, 0xd08, 0x15, BIT3, (P->DelayFlags & 2) ? BIT3 : 0);
  }

  // PHYCR1/2 are the default-page RTL8211F controls, independent of delay page.
  if (!EFI_ERROR (Status) && P->Aldps) {
    Status = PagedModify (P, 0, 0x18, BIT1 | BIT2 | BIT12, BIT1 | BIT2 | BIT12);
  }

  if (!EFI_ERROR (Status) && P->DisableClkout) {
    Status = PagedModify (P, 0, 0x19, BIT0, 0);
    if (!EFI_ERROR (Status)) {
      Status = SoftReset (P);
    }
  }
  return Status;
}

STATIC EFI_STATUS
ConfigureYt8531 (EQOS_PORT *P)
{
  UINT16 Saved, Delay;
  EFI_STATUS Status = SoftReset (P);

  if (EFI_ERROR (Status)) { return Status; }
  Status = EqosMdioRead (P, YT8531_EXT_INDEX, &Saved);
  if (EFI_ERROR (Status)) { return Status; }

  // EQoS needs RGMII RXC to finish a DMA reset even with no link.
  Status = EqosMdioWrite (P, YT8531_EXT_INDEX, YT8531_SLEEP_CONTROL);
  if (!EFI_ERROR (Status)) {
    Status = Modify (P, YT8531_EXT_DATA, YT8531_AUTO_SLEEP, 0);
  }
  if (!EFI_ERROR (Status)) {
    Status = EqosMdioWrite (P, YT8531_EXT_INDEX, YT8531_CLOCK_GATING);
  }
  if (!EFI_ERROR (Status)) {
    Status = Modify (P, YT8531_EXT_DATA, YT8531_GATE_RXC_LINK_DOWN, 0);
  }
  // Use the PHY's 150 ps delay taps. Clear its extra 1.9 ns RX delay.
  if (!EFI_ERROR (Status)) {
    Status = EqosMdioWrite (P, YT8531_EXT_INDEX, YT8531_CHIP_CONFIG);
  }
  if (!EFI_ERROR (Status)) { Status = Modify (P, YT8531_EXT_DATA, BIT8, 0); }
  if (!EFI_ERROR (Status)) {
    Status = EqosMdioWrite (P, YT8531_EXT_INDEX, YT8531_RGMII_CONFIG);
  }
  if (!EFI_ERROR (Status)) {
    Delay = (P->DelayFlags & BIT0) ? YT8531_DELAY_1950_PS : 0;
    if (P->DelayFlags & BIT1) { Delay |= YT8531_DELAY_1950_PS << 10; }
    Status = Modify (P, YT8531_EXT_DATA, (0xfU << 10) | 0xfU, Delay);
  }
  return RestoreSelector (P, YT8531_EXT_INDEX, Saved, Status);
}

EFI_STATUS
EqosPhyConfigure (
  EQOS_PORT  *P
  )
{
  UINT16  Id1, Id2;

  if (!P || !P->Ready || P->Faulted || P->PhyConfigured) {
    return EFI_NOT_READY;
  }

  EFI_STATUS  Status = EqosMdioRead (P, 2, &Id1);

  if (!EFI_ERROR (Status)) {
    Status = EqosMdioRead (P, 3, &Id2);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  P->PhyId = ((UINT32)Id1 << 16) | Id2;
  if (P->PhyId == YT8531_ID) {
    Status = ConfigureYt8531 (P);
  } else if (P->PhyId == RTL8211F_ID) {
    Status = ConfigureRtl8211f (P);
  } else {
    return EFI_UNSUPPORTED;
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Advertise 10/100 and 1000 full-duplex; never claim a link before negotiation.
  Status = Modify (P, 4, 0x0fe0, 0x0de0);
  if (!EFI_ERROR (Status)) {
    Status = Modify (P, 9, BIT8 | BIT9, BIT9);
  }

  if (!EFI_ERROR (Status)) {
    Status = Modify (P, 0, BIT12 | BIT9 | BIT11 | BIT10, BIT12 | BIT9);
  }

  if (!EFI_ERROR (Status)) {
    P->PhyConfigured = TRUE;
  }

  return Status;
}

EFI_STATUS
EqosLink (
  EQOS_PORT  *P,
  BOOLEAN       *Up,
  BOOLEAN       *FullDuplex,
  UINT32        *Speed
  )
{
  UINT16  Bmsr, Saved, PhyStatus;

  if (!P || !Up || !FullDuplex || !Speed) {
    return EFI_INVALID_PARAMETER;
  }

  *Up    = *FullDuplex = FALSE;
  *Speed = 0;
  if (!P->PhyConfigured || P->Faulted) {
    return EFI_NOT_READY;
  }

  EFI_STATUS  Status = EqosMdioRead (P, 1, &Bmsr);

  if (!EFI_ERROR (Status)) {
    Status = EqosMdioRead (P, 1, &Bmsr);
  }

  if (EFI_ERROR (Status) || !(Bmsr & BIT2) || !(Bmsr & BIT5)) {
    return Status;
  }

  UINT16 SpeedCode;
  BOOLEAN Duplex;
  if (P->PhyId == YT8531_ID) {
    Status = EqosMdioRead (P, YT8531_STATUS, &PhyStatus);
    if (EFI_ERROR (Status) || !(PhyStatus & BIT10)) { return Status; }
    SpeedCode = (PhyStatus >> 14) & 3;
    Duplex = (PhyStatus & BIT13) != 0;
  } else {
    Status = EqosMdioRead (P, 31, &Saved);
    if (EFI_ERROR (Status)) { return Status; }
    Status = EqosMdioWrite (P, 31, 0xa43);
    if (!EFI_ERROR (Status)) { Status = EqosMdioRead (P, 0x1a, &PhyStatus); }
    Status = RestoreSelector (P, 31, Saved, Status);
    if (EFI_ERROR (Status) || !(PhyStatus & BIT2)) { return Status; }
    SpeedCode = (PhyStatus >> 4) & 3;
    Duplex = (PhyStatus & BIT3) != 0;
  }
  switch (SpeedCode) {
    case 0: *Speed = 10;
      break;
    case 1: *Speed = 100;
      break;
    case 2: *Speed = 1000;
      break;
    default: return EFI_DEVICE_ERROR;
  }

  *FullDuplex = Duplex;
  *Up         = TRUE;
  return EFI_SUCCESS;
}
