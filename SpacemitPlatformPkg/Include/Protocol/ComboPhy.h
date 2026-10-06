/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef SPACEMIT_COMBO_PHY_PROTOCOL_H
#define SPACEMIT_COMBO_PHY_PROTOCOL_H
#include <Uefi.h>
#define SPACEMIT_COMBO_PHY_REVISION  2U
typedef struct {
  UINT32    Revision;
  INT32     Node;
  EFI_STATUS (EFIAPI *Pcie)(CONST VOID *Fdt, INT32 Consumer, UINT32 Lanes);
  // Appended in revision 2; the revision 1 PCIe prefix remains unchanged.
  EFI_STATUS (EFIAPI *Usb3)(CONST VOID *Fdt, INT32 Consumer);
} SPACEMIT_COMBO_PHY_PROTOCOL;
extern EFI_GUID  gSpacemitComboPhyProtocolGuid;
#endif
