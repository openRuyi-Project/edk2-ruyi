/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef FU740_INFO_LIB_H
#define FU740_INFO_LIB_H
#include <Library/FdtBoardInfoLib.h>
typedef RUYI_BOARD_INFO FU740_BOARD_INFO;
EFI_STATUS
Fu740GetBoardInfo (
  FU740_BOARD_INFO  *Info
  );
#endif
