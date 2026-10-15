/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_SPI_NOR_LIB_H_
#define K3_SPI_NOR_LIB_H_
#include <Uefi.h>
#include <Protocol/SpiNorFlash.h>

EFI_STATUS
K3SpiNorFindFlash (
  EFI_SPI_NOR_FLASH_PROTOCOL  **Nor
  );

#endif
