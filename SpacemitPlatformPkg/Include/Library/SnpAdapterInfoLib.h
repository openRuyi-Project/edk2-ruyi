/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef SNP_ADAPTER_INFO_LIB_H
#define SNP_ADAPTER_INFO_LIB_H
#include <Uefi.h>
#include <Protocol/AdapterInformation.h>
#include <Protocol/SimpleNetwork.h>

typedef struct {
  UINT32                              Signature;
  EFI_ADAPTER_INFORMATION_PROTOCOL    Protocol;
  EFI_SIMPLE_NETWORK_PROTOCOL         *Snp;
} SNP_ADAPTER_INFO;

EFI_STATUS
SnpAdapterInfoInitialize (
  SNP_ADAPTER_INFO             *Info,
  EFI_SIMPLE_NETWORK_PROTOCOL  *Snp
  );

#endif
