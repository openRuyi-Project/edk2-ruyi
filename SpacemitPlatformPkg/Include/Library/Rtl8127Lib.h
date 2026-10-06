/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef RTL8127_LIB_H
#define RTL8127_LIB_H
#include <Uefi.h>
#include <Protocol/PciIo.h>

/* Caller owns the PCI function, validates BAR2 >= 0x2351 bytes and enables
   memory decoding. No function below enables bus mastering or packet DMA. */
EFI_STATUS
Rtl8127InitializeSfp (
  IN EFI_PCI_IO_PROTOCOL  *Pci
  );

#endif
