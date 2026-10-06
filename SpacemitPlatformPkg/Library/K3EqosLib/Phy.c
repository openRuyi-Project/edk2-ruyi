/** SPDX-License-Identifier: BSD-2-Clause-Patent */

/* Clause 22 transaction reference: BSD SpacemiT DwEqosDxeUtil.
 * Copyright (c) 2025, Spacemit Corporation. All rights reserved.
 * RTL8211F register/mode semantics checked against the pinned PHY sources.
 */
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/K3EqosLib.h>
#include <Library/TimerLib.h>

STATIC EFI_STATUS
WaitIdle (
  K3_EQOS_PORT  *P
  )
{
  for (UINTN Elapsed = 0; Elapsed <= 10000; Elapsed += 10) {
    if (!(MmioRead32 (P->Base + 0x200) & BIT0)) {
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
  K3_EQOS_PORT  *P,
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
    MmioWrite32 (P->Base + 0x204, *Value);
  }

  // Always construct a Clause 22 command; never inherit C45/preamble state.
  UINT32  Command = (P->PhyAddress << 21) | (Register << 16) | ((UINT32)P->MdioRange << 8) |
                    ((Write ? 1U : 3U) << 2) | BIT0;

  MmioWrite32 (P->Base + 0x200, Command);
  Status = WaitIdle (P);
  if (!EFI_ERROR (Status) && !Write) {
    *Value = (UINT16)MmioRead32 (P->Base + 0x204);
  }

  return Status;
}

EFI_STATUS
K3EqosMdioRead (
  K3_EQOS_PORT  *P,
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
K3EqosMdioWrite (
  K3_EQOS_PORT  *P,
  UINT32        Register,
  UINT16        Value
  )
{
  return Transfer (P, Register, &Value, TRUE);
}

STATIC EFI_STATUS
Modify (
  K3_EQOS_PORT  *P,
  UINT32        Register,
  UINT16        Mask,
  UINT16        Set
  )
{
  UINT16      Value;
  EFI_STATUS  Status = K3EqosMdioRead (P, Register, &Value);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  UINT16  New = (Value & ~Mask) | (Set & Mask);

  return New == Value ? EFI_SUCCESS : K3EqosMdioWrite (P, Register, New);
}

STATIC EFI_STATUS
RestorePage (
  K3_EQOS_PORT  *P,
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

  EFI_STATUS  Status = K3EqosMdioWrite (P, 31, Saved);

  if (EFI_ERROR (Status)) {
    P->RestoreError = Status;
    P->Faulted      = TRUE;
    return EFI_ERROR (Original) ? Original : Status;
  }

  return Original;
}

STATIC EFI_STATUS
PagedModify (
  K3_EQOS_PORT  *P,
  UINT16        Page,
  UINT32        Register,
  UINT16        Mask,
  UINT16        Set
  )
{
  UINT16      Saved;
  EFI_STATUS  Status = K3EqosMdioRead (P, 31, &Saved);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3EqosMdioWrite (P, 31, Page);
  if (!EFI_ERROR (Status)) {
    Status = Modify (P, Register, Mask, Set);
  }

  return RestorePage (P, Saved, Status);
}

STATIC EFI_STATUS
SoftReset (
  K3_EQOS_PORT  *P
  )
{
  EFI_STATUS  Status = Modify (P, 0, BIT15, BIT15);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  UINT16  Value;

  for (UINTN Elapsed = 0; Elapsed <= 500000; Elapsed += 5000) {
    Status = K3EqosMdioRead (P, 0, &Value);
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

EFI_STATUS
K3EqosPhyConfigure (
  K3_EQOS_PORT  *P
  )
{
  UINT16  Id1, Id2;

  if (!P || !P->Ready || P->Faulted || P->PhyConfigured) {
    return EFI_NOT_READY;
  }

  EFI_STATUS  Status = K3EqosMdioRead (P, 2, &Id1);

  if (!EFI_ERROR (Status)) {
    Status = K3EqosMdioRead (P, 3, &Id2);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  P->PhyId = ((UINT32)Id1 << 16) | Id2;
  if (P->PhyId != 0x001cc916) {
    return EFI_UNSUPPORTED;
  }

  Status = SoftReset (P);
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
K3EqosLink (
  K3_EQOS_PORT  *P,
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

  EFI_STATUS  Status = K3EqosMdioRead (P, 1, &Bmsr);

  if (!EFI_ERROR (Status)) {
    Status = K3EqosMdioRead (P, 1, &Bmsr);
  }

  if (EFI_ERROR (Status) || !(Bmsr & BIT2) || !(Bmsr & BIT5)) {
    return Status;
  }

  Status = K3EqosMdioRead (P, 31, &Saved);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3EqosMdioWrite (P, 31, 0xa43);
  if (!EFI_ERROR (Status)) {
    Status = K3EqosMdioRead (P, 0x1a, &PhyStatus);
  }

  Status = RestorePage (P, Saved, Status);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!(PhyStatus & BIT2)) {
    return EFI_SUCCESS;
  }

  switch ((PhyStatus >> 4) & 3) {
    case 0: *Speed = 10;
      break;
    case 1: *Speed = 100;
      break;
    case 2: *Speed = 1000;
      break;
    default: return EFI_DEVICE_ERROR;
  }

  *FullDuplex = (PhyStatus & BIT3) != 0;
  *Up         = TRUE;
  return EFI_SUCCESS;
}
