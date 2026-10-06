/** SPDX-License-Identifier: BSD-2-Clause-Patent */

/* Display sequencing uses the shared clock/reset provider and native
   APMU device power domains. */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/K3DisplayClockLib.h>
#include <Library/K3HardwareLib.h>
#include <Library/UefiBootServicesTableLib.h>
STATIC EFI_STATUS
ParsePower (
  K3_DISPLAY_CLOCK  *C
  )
{
  CONST VOID    *Fdt = C->Resources.Fdt;
  INT32         Bytes;
  CONST UINT32  *Dpu = FdtGetProp (Fdt, C->Resources.DpuNode, "power-domains", &Bytes);

  if (!Dpu || (Bytes != 2 * sizeof (UINT32))) {
    return EFI_NOT_FOUND;
  }

  UINT32  ProviderId = Fdt32ToCpu (Dpu[0]);
  UINT32  Domain     = Fdt32ToCpu (Dpu[1]);
  INT32   Provider   = FdtNodeOffsetByPhandle (Fdt, ProviderId);

  if (!K3FdtEnabled (Fdt, Provider) ||
      (FdtStringListSearch (Fdt, Provider, "compatible", "spacemit,k3-syscon-apmu") < 0))
  {
    return EFI_UNSUPPORTED;
  }

  CONST UINT32  *Dp = FdtGetProp (Fdt, C->Resources.DpNode, "power-domains", &Bytes);

  if (!Dp || (Bytes != 2 * sizeof (UINT32)) ||
      (Fdt32ToCpu (Dp[0]) != ProviderId) || (Fdt32ToCpu (Dp[1]) != Domain))
  {
    return EFI_PROTOCOL_ERROR;
  }

  CONST UINT32  *Cells = FdtGetProp (Fdt, Provider, "#power-domain-cells", &Bytes);

  if (!Cells || (Bytes != sizeof (UINT32)) || (Fdt32ToCpu (*Cells) != 1)) {
    return EFI_PROTOCOL_ERROR;
  }

  if (Domain >= K3_PMDOMAIN_COUNT) {
    return EFI_UNSUPPORTED;
  }

  C->PowerDomain = Domain;
  return K3PowerDomainInit (Fdt, Provider, &C->Power);
}

EFI_STATUS
K3DisplayClockParse (
  CONST K3_DPU_RESOURCES  *R,
  K3_DISPLAY_CLOCK        *C
  )
{
  if (!R || !C || !R->Fdt || (R->Dpu != 0xc0440000) || (R->Dp != 0xcac88000) || C->Owned) {
    return EFI_INVALID_PARAMETER;
  }

  K3_DISPLAY_CLOCK  P = { .Resources = *R };
  EFI_STATUS        S = gBS->LocateProtocol (&gSpacemitClockProtocolGuid, NULL, (VOID **)&P.Clock);

  if (EFI_ERROR (S) || !P.Clock || (P.Clock->Revision != 1)) {
    return EFI_NOT_READY;
  }

  STATIC CONST CHAR8  *Names[]  = { "pxclk", "mclk", "aclk", "escclk", "dscclk" };
  STATIC CONST CHAR8  *Resets[] = { "mclk", "lcd", "esc", "aclk", "dsc" };

  for (UINTN I = 0; I < 5; I++) {
    S = P.Clock->Resolve (R->Fdt, R->DpuNode, Names[I], FALSE, &P.Clocks[I]);
    if (!EFI_ERROR (S)) {
      S = P.Clock->Resolve (R->Fdt, R->DpuNode, Resets[I], TRUE, &P.Resets[I]);
    }

    if (EFI_ERROR (S)) {
      return S;
    }
  }

  S = P.Clock->Resolve (R->Fdt, R->DpNode, "pxclk", FALSE, &P.Clocks[5]);
  if (!EFI_ERROR (S)) {
    S = P.Clock->Resolve (R->Fdt, R->DpNode, NULL, TRUE, &P.Resets[5]);
  }

  if (!EFI_ERROR (S)) {
    S = K3MapMmio (R->Apmu, 0x3f8);
  }

  if (EFI_ERROR (S)) {
    return S;
  }

  S = ParsePower (&P);
  if (EFI_ERROR (S)) {
    return S;
  }

  P.Parsed = TRUE;
  *C       = P;
  return EFI_SUCCESS;
}

EFI_STATUS
K3DisplayChooseClock (
  K3_DISPLAY_CLOCK       *C,
  UINT64                 Hz,
  K3_DISPLAY_CLOCK_PLAN  *Plan
  )
{
  if (!C || !C->Parsed || !Plan || !Hz || (Hz > 750000000)) {
    return EFI_INVALID_PARAMETER;
  }

  K3_DISPLAY_CLOCK_PLAN  P = { .RequestedHz = Hz };
  EFI_STATUS             S = C->Clock->RoundRate (C->Clocks[0], Hz, &P.ActualHz, &P.Selector, &P.Divider);

  if (!EFI_ERROR (S)) {
    *Plan = P;
  }

  return S;
}

STATIC EFI_STATUS
Hold (
  K3_DISPLAY_CLOCK  *C
  )
{
  EFI_STATUS  Result = EFI_SUCCESS;

  for (UINTN I = 0; I < 6; I++) {
    EFI_STATUS  S = C->Clock->Reset (C->Resets[I], TRUE);
    if (EFI_ERROR (S)) {
      Result = S;
    }
  }

  return Result;
}

EFI_STATUS
K3DisplayColdAbort (
  K3_DISPLAY_CLOCK  *C
  )
{
  if (!C || !C->Owned) {
    return EFI_INVALID_PARAMETER;
  }

  if (C->DmaStarted) {
    return EFI_ACCESS_DENIED;
  }

  C->Ready = FALSE;
  EFI_STATUS  S = Hold (C);

  if (EFI_ERROR (S)) {
    C->Faulted = TRUE;
    return S;
  }

  for (UINTN I = 0; I < 6; I++) {
    S = C->Clock->Enable (C->Clocks[I], FALSE);
    if (EFI_ERROR (S)) {
      C->Faulted = TRUE;
      return S;
    }
  }

  if (C->PowerAdded) {
    S = K3PowerDomainSet (&C->Power, C->PowerDomain, FALSE);
    if (EFI_ERROR (S)) {
      C->Faulted = TRUE;
      return S;
    }

    BOOLEAN  State;

    S = K3PowerDomainGet (&C->Power, C->PowerDomain, &State);
    if (EFI_ERROR (S) || State) {
      C->Faulted = TRUE;
      return EFI_ERROR (S) ? S : EFI_DEVICE_ERROR;
    }
  }

  C->Owned = C->Faulted = C->PowerAdded = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS
K3DisplayPrepare (
  K3_DISPLAY_CLOCK             *C,
  CONST K3_DISPLAY_CLOCK_PLAN  *P
  )
{
  if (!C || !C->Parsed || !P) {
    return EFI_INVALID_PARAMETER;
  }

  if (C->Owned || C->DmaStarted) {
    return EFI_ALREADY_STARTED;
  }

  if (C->Faulted) {
    return EFI_NOT_READY;
  }

  K3_DISPLAY_CLOCK_PLAN  Check;
  EFI_STATUS             S = K3DisplayChooseClock (C, P->RequestedHz, &Check);

  if (EFI_ERROR (S) || (Check.ActualHz != P->ActualHz) || (Check.Selector != P->Selector) ||
      (Check.Divider != P->Divider))
  {
    return EFI_NOT_READY;
  }

  for (UINTN I = 0; I < 6; I++) {
    if (C->Clock->IsEnabled (C->Clocks[I])) {
      return EFI_ACCESS_DENIED;
    }
  }

  BOOLEAN  State;

  S = K3PowerDomainGet (&C->Power, C->PowerDomain, &State);
  if (EFI_ERROR (S)) {
    goto BeforeOwnership;
  }

  S = K3ApplyPins (C->Resources.Fdt, C->Resources.DpNode);
  if (EFI_ERROR (S)) {
    goto BeforeOwnership;
  }

  C->PowerAdded = !State;
  C->Owned      = TRUE;
  if (C->PowerAdded) {
    S = K3PowerDomainSet (&C->Power, C->PowerDomain, TRUE);
    if (EFI_ERROR (S)) {
      goto Fail;
    }
  }

  S = Hold (C);
  if (EFI_ERROR (S)) {
    goto Fail;
  }

  UINT64  Rates[] = { P->ActualHz, 307200000, 409600000, 51200000, 76800000 };

  for (UINTN I = 0; I < 5; I++) {
    UINT64  Actual;
    S = C->Clock->SetRate (C->Clocks[I], Rates[I], &Actual);
    if (!EFI_ERROR (S) && (Actual != Rates[I])) {
      S = EFI_UNSUPPORTED;
    }

    if (!EFI_ERROR (S)) {
      S = C->Clock->Enable (C->Clocks[I], TRUE);
    }

    if (EFI_ERROR (S)) {
      goto Fail;
    }
  }

  S = C->Clock->SetParent (C->Clocks[5], 1);
  if (!EFI_ERROR (S)) {
    S = C->Clock->Enable (C->Clocks[5], TRUE);
  }

  if (EFI_ERROR (S)) {
    goto Fail;
  }

  for (UINTN I = 0; I < 6; I++) {
    S = C->Clock->Reset (C->Resets[I], FALSE);
    if (EFI_ERROR (S)) {
      goto Fail;
    }
  }

  C->Pixel = *P;
  C->Ready = TRUE;
  UINT64  Actual;

  S = K3DisplayGetPixelClock (C, &Actual);
  if (!EFI_ERROR (S) && (Actual == P->ActualHz)) {
    return EFI_SUCCESS;
  }

  if (!EFI_ERROR (S)) {
    S = EFI_DEVICE_ERROR;
  }

Fail:
  C->Faulted = TRUE;
  K3DisplayColdAbort (C);
  return S;

BeforeOwnership:
  return S;
}

EFI_STATUS
K3DisplayGetPixelClock (
  VOID    *Priv,
  UINT64  *Hz
  )
{
  K3_DISPLAY_CLOCK  *C = Priv;

  if (!C || !Hz || !C->Owned || !C->Ready || C->Faulted) {
    return EFI_NOT_READY;
  }

  BOOLEAN     State;
  EFI_STATUS  PowerStatus = K3PowerDomainGet (&C->Power, C->PowerDomain, &State);

  if (EFI_ERROR (PowerStatus) || !State) {
    return EFI_ERROR (PowerStatus) ? PowerStatus : EFI_NOT_READY;
  }

  for (UINTN I = 0; I < 6; I++) {
    if (!C->Clock->IsEnabled (C->Clocks[I]) || C->Clock->IsResetAsserted (C->Resets[I])) {
      return EFI_NOT_READY;
    }
  }

  STATIC CONST UINT64  Rates[] = { 307200000, 409600000, 51200000, 76800000 };

  for (UINTN I = 1; I < 5; I++) {
    UINT64      Actual;
    EFI_STATUS  S = C->Clock->GetRate (C->Clocks[I], &Actual);
    if (EFI_ERROR (S) || (Actual != Rates[I - 1])) {
      return EFI_NOT_READY;
    }
  }

  return C->Clock->GetRate (C->Clocks[0], Hz);
}

EFI_STATUS
K3DisplayMarkDma (
  K3_DISPLAY_CLOCK  *C
  )
{
  UINT64      Hz;
  EFI_STATUS  S = K3DisplayGetPixelClock (C, &Hz);

  if (!EFI_ERROR (S)) {
    C->DmaStarted = TRUE;
  }

  return S;
}
