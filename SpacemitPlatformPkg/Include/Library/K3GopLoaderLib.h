/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_GOP_LOADER_LIB_H_
#define K3_GOP_LOADER_LIB_H_
#include <Uefi.h>
EFI_STATUS
K3GopInitialize (
  VOID
  );

VOID
K3GopConnectSelectedDrivers (
  VOID
  );

#endif
