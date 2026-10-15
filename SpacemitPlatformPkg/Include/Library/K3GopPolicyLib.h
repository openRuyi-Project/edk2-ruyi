/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_GOP_POLICY_LIB_H_
#define K3_GOP_POLICY_LIB_H_
#include <Uefi.h>
#include <IndustryStandard/Pci.h>

extern EFI_GUID  gK3GopPolicyReadyGuid;

typedef enum {
  K3GopX64 = 0,
  K3GopAmdPreSoc15
} K3_GOP_POLICY;

K3_GOP_POLICY
K3GopPolicyForDevice (
  UINT16  VendorId,
  UINT16  DeviceId
  );

EFI_STATUS
K3GopReadPciDevice (
  EFI_HANDLE  Controller,
  PCI_TYPE00  *Pci
  );

UINT16
K3GopImageMachine (
  CONST VOID  *Image,
  UINTN       Size
  );

CONST EFI_GUID *
K3GopFileGuid (
  K3_GOP_POLICY  Policy
  );

K3_GOP_POLICY
K3GopFilePolicy (
  CONST EFI_DEVICE_PATH_PROTOCOL  *Path
  );

BOOLEAN
K3GopVerifyImage (
  K3_GOP_POLICY  Policy,
  CONST VOID     *Image,
  UINTN          Size
  );

#endif
