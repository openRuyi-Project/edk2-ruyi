/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef PCI_GOP_LOADER_LIB_H_
#define PCI_GOP_LOADER_LIB_H_
#include <Uefi.h>
EFI_STATUS
PciGopInitialize (
  VOID
  );

VOID
PciGopConnectEmulator (
  VOID
  );

VOID
PciGopConnectSelectedDrivers (
  VOID
  );

#endif
