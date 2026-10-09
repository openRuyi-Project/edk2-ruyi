/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/K3HardwareLib.h>
#include <Library/K3PowerDomainLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define POWER_STATUS     0xf0
#define POWER_REQUEST    BIT0
#define POWER_ISOLATION  BIT1
#define POWER_SLEEP1     BIT2
#define POWER_SLEEP2     BIT3
#define POWER_HW_MODE    BIT4

STATIC CONST struct {
  UINT32     Control, Status;
  BOOLEAN    Hardware;
} mDomains[K3_PMDOMAIN_COUNT] = {
  [K3_PMDOMAIN_VPU]   = { 0x0a8, BIT9,  TRUE  },
  [K3_PMDOMAIN_GPU]   = { 0x0d0, BIT8,  TRUE  },
  [K3_PMDOMAIN_AUDIO] = { 0x378, BIT3,  FALSE },
  [K3_PMDOMAIN_LCD0]  = { 0x380, BIT12, TRUE  },
  [K3_PMDOMAIN_LCD1]  = { 0x3f4, BIT15, TRUE  }
};

EFI_STATUS
K3PowerDomainInit (
  CONST VOID       *Fdt,
  INT32            Provider,
  K3_POWER_DOMAIN  *Power
  )
{
  if (!Fdt || !Power || !K3FdtEnabled (Fdt, Provider) ||
      (FdtStringListSearch (Fdt, Provider, "compatible", "spacemit,k3-syscon-apmu") < 0))
  {
    return EFI_UNSUPPORTED;
  }

  UINTN       Base;
  EFI_STATUS  Status = K3FdtRegister (Fdt, Provider, 0, 0x400, &Base, NULL);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Base != 0xd4282800) {
    return EFI_UNSUPPORTED;
  }

  Status = K3MapMmio (Base, 0x400);
  if (!EFI_ERROR (Status)) {
    Power->Apmu = Base;
  }

  return Status;
}

EFI_STATUS
K3PowerDomainGet (
  K3_POWER_DOMAIN  *Power,
  UINT32           Domain,
  BOOLEAN          *On
  )
{
  if (!Power || !Power->Apmu || (Domain >= K3_PMDOMAIN_COUNT) || !On) {
    return EFI_INVALID_PARAMETER;
  }

  *On = (MmioRead32 (Power->Apmu + POWER_STATUS) & mDomains[Domain].Status) != 0;
  return EFI_SUCCESS;
}

EFI_STATUS
K3PowerDomainSet (
  K3_POWER_DOMAIN  *Power,
  UINT32           Domain,
  BOOLEAN          On
  )
{
  BOOLEAN     Current;
  EFI_STATUS  Status = K3PowerDomainGet (Power, Domain, &Current);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Domain == K3_PMDOMAIN_AUDIO) && !On) {
    return EFI_ACCESS_DENIED;
  }

  UINTN   Control = Power->Apmu + mDomains[Domain].Control;
  UINT32  Mask    = POWER_REQUEST | POWER_HW_MODE;

  if ((Current == On) && (!mDomains[Domain].Hardware ||
                          ((MmioRead32 (Control) & Mask) == (On ? Mask : 0))))
  {
    return EFI_SUCCESS;
  }

  EFI_TPL  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);

  if (mDomains[Domain].Hardware) {
    MmioAndThenOr32 (Control, ~Mask, On ? Mask : 0);
    MicroSecondDelay (On ? 300 : 15);
    if ((MmioRead32 (Control) & Mask) != (On ? Mask : 0)) {
      gBS->RestoreTPL (OldTpl);
      return EFI_DEVICE_ERROR;
    }
  } else {
    // AUDIO is always on and uses the software power-up sequence.
    MmioOr32 (Control, POWER_SLEEP1);
    MicroSecondDelay (20);
    MmioOr32 (Control, POWER_SLEEP1 | POWER_SLEEP2);
    MicroSecondDelay (20);
    MmioOr32 (Control, POWER_ISOLATION);
    MicroSecondDelay (15);
  }

  Status = EFI_TIMEOUT;
  for (UINTN Elapsed = 0; Elapsed <= 10000; Elapsed += 5) {
    if (((MmioRead32 (Power->Apmu + POWER_STATUS) & mDomains[Domain].Status) != 0) == On) {
      Status = EFI_SUCCESS;
      break;
    }

    MicroSecondDelay (5);
  }

  gBS->RestoreTPL (OldTpl);
  return Status;
}
