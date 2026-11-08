/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef EIC7700_INFO_LIB_H
#define EIC7700_INFO_LIB_H
#include <Library/FdtBoardInfoLib.h>
typedef RUYI_BOARD_INFO EIC7700_BOARD_INFO;
EFI_STATUS
Eic7700GetBoardInfo (
  EIC7700_BOARD_INFO  *Info
  );
#endif
