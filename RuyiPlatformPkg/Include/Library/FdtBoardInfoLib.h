/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef FDT_BOARD_INFO_LIB_H
#define FDT_BOARD_INFO_LIB_H
#include <Uefi.h>
typedef struct {
  CONST CHAR8    *Model;
  UINT64         MemoryBase;
  UINT64         MemorySize;
  UINT64         BootHart;
  UINT64         CpuRate;
  UINT32         CoreCount;
} RUYI_BOARD_INFO;
EFI_STATUS
FdtBoardInfoGet (
  RUYI_BOARD_INFO  *Info,
  CONST VOID       **DeviceTree OPTIONAL
  );

#endif
