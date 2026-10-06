/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef RTL8127_SNP_LIB_H
#define RTL8127_SNP_LIB_H
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/SimpleNetwork.h>
EFI_STATUS
Rtl8127SnpCreate (
  EFI_PCI_IO_PROTOCOL          *Pci,
  CONST UINT8                  Mac[6],
  EFI_SIMPLE_NETWORK_PROTOCOL  **Snp
  );

/* Destruction succeeds only after packet DMA and its mappings are stopped. */
EFI_STATUS
Rtl8127SnpDestroy (
  EFI_SIMPLE_NETWORK_PROTOCOL  **Snp
  );

EFI_STATUS
Rtl8127SnpExit (
  EFI_SIMPLE_NETWORK_PROTOCOL  *Snp
  );

#endif
