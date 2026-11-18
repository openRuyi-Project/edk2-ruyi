// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/Eic7700BoardProfileLib.h>

CONST EIC7700_BOARD_PROFILE *
Eic7700GetBoardProfile (VOID)
{
  STATIC CONST EIC7700_BOARD_PROFILE Profile = {
    {"pine64,starpro64", "CONFIG_BOARD_PINE64_STARPRO64=y", NULL, 0x82200000, SIZE_4MB},
    "PINE64", 0xf80000, 0x80000, FALSE
  };
  return &Profile;
}
