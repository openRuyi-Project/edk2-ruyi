/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Guid/RiscVSecHobData.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "Core.h"
STATIC SPACEMIT_CLOCK_PROTOCOL  *mCore;
STATIC EFI_STATUS
EFIAPI
Rate (
  UINT32  Id,
  UINT64  Requested,
  UINT64  *Actual
  )
{
  EFI_TPL     Old = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  S   = mCore->SetRate (Id, Requested, Actual);

  gBS->RestoreTPL (Old);
  return S;
}

STATIC EFI_STATUS
EFIAPI
Gate (
  UINT32   Id,
  BOOLEAN  On
  )
{
  EFI_TPL     Old = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  S   = mCore->Enable (Id, On);

  gBS->RestoreTPL (Old);
  return S;
}

STATIC EFI_STATUS
EFIAPI
Reset (
  UINT32   Id,
  BOOLEAN  Assert
  )
{
  EFI_TPL     Old = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  S   = mCore->Reset (Id, Assert);

  gBS->RestoreTPL (Old);
  return S;
}

STATIC EFI_STATUS
EFIAPI
Parent (
  UINT32  Id,
  UINT32  Index
  )
{
  EFI_TPL     Old = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  S   = mCore->SetParent (Id, Index);

  gBS->RestoreTPL (Old);
  return S;
}

EFI_STATUS
EFIAPI
K3ClockEntry (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  SPACEMIT_CLOCK_PROTOCOL  *Protocol;

  if (!EFI_ERROR (gBS->LocateProtocol (&gSpacemitClockProtocolGuid, NULL, (VOID **)&Protocol))) {
    return EFI_ALREADY_STARTED;
  }

  CONST EFI_GUID          Guid = RISCV_SEC_HANDOFF_HOB_GUID;
  EFI_HOB_GUID_TYPE       *Hob = GetFirstGuidHob (&Guid);
  RISCV_SEC_HANDOFF_DATA  Handoff;

  if (!Hob || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (Handoff))) {
    return EFI_NOT_FOUND;
  }

  CopyMem (&Handoff, GET_GUID_HOB_DATA (Hob), sizeof (Handoff));
  if (!Handoff.FdtPointer || FdtCheckHeader (Handoff.FdtPointer)) {
    return EFI_COMPROMISED_DATA;
  }

  EFI_STATUS  S = K3ClockCoreInitialize (Handoff.FdtPointer, &Protocol);

  if (EFI_ERROR (S)) {
    return S;
  }

  mCore = Protocol;
  STATIC SPACEMIT_CLOCK_PROTOCOL  Public;

  Public           = *Protocol;
  Public.SetRate   = Rate;
  Public.Enable    = Gate;
  Public.Reset     = Reset;
  Public.SetParent = Parent;
  EFI_HANDLE  Handle = NULL;

  return gBS->InstallProtocolInterface (&Handle, &gSpacemitClockProtocolGuid, EFI_NATIVE_INTERFACE, &Public);
}
