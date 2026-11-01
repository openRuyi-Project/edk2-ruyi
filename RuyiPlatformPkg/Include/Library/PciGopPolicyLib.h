/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef PCI_GOP_POLICY_LIB_H_
#define PCI_GOP_POLICY_LIB_H_
#include <Uefi.h>
#include <IndustryStandard/Pci.h>

extern EFI_GUID  gPciGopPolicyReadyGuid;

typedef enum {
  PciGopX64 = 0,
  PciGopAmdPreSoc15
} PCI_GOP_POLICY;

PCI_GOP_POLICY
PciGopPolicyForDevice (
  UINT16  VendorId,
  UINT16  DeviceId
  );

EFI_STATUS
PciGopReadPciDevice (
  EFI_HANDLE  Controller,
  PCI_TYPE00  *Pci
  );

UINT16
PciGopImageMachine (
  CONST VOID  *Image,
  UINTN       Size
  );

CONST EFI_GUID *
PciGopFileGuid (
  PCI_GOP_POLICY  Policy
  );

PCI_GOP_POLICY
PciGopFilePolicy (
  CONST EFI_DEVICE_PATH_PROTOCOL  *Path
  );

BOOLEAN
PciGopVerifyImage (
  PCI_GOP_POLICY  Policy,
  CONST VOID      *Image,
  UINTN           Size
  );

#endif
