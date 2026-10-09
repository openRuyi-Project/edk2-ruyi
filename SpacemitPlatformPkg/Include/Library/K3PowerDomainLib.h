/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>

#define K3_PMDOMAIN_VPU    0
#define K3_PMDOMAIN_GPU    1
#define K3_PMDOMAIN_AUDIO  2
#define K3_PMDOMAIN_LCD0   3
#define K3_PMDOMAIN_LCD1   4
#define K3_PMDOMAIN_COUNT  5

typedef struct {
  UINTN    Apmu;
} K3_POWER_DOMAIN;

EFI_STATUS
K3PowerDomainInit (
  CONST VOID       *Fdt,
  INT32            Provider,
  K3_POWER_DOMAIN  *Power
  );

EFI_STATUS
K3PowerDomainGet (
  K3_POWER_DOMAIN  *Power,
  UINT32           Domain,
  BOOLEAN          *On
  );

EFI_STATUS
K3PowerDomainSet (
  K3_POWER_DOMAIN  *Power,
  UINT32           Domain,
  BOOLEAN          On
  );
