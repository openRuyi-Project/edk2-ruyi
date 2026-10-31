/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef FU740_INFO_LIB_H
#define FU740_INFO_LIB_H
#include <Uefi.h>
typedef struct {
  CONST CHAR8    *Model;
  UINT64         MemoryBase;
  UINT64         MemorySize;
  UINT64         BootHart;
  UINT64         CpuRate;
  UINT32         CoreCount;
} FU740_BOARD_INFO;
EFI_STATUS
Fu740GetBoardInfo (
  FU740_BOARD_INFO  *Info
  );

#endif
