// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/Eic7700BoardProfileLib.h>

CONST EIC7700_BOARD_PROFILE *
Eic7700GetBoardProfile (VOID)
{
  STATIC CONST EIC7700_BOARD_PROFILE Profile = {
    {"milkv,megrez", "CONFIG_BOARD_MILKV_MEGREZ=y", NULL, 0x82200000, SIZE_4MB},
    "Milk-V", 0x80000000, 0x780000, 0x80000, FALSE
  };
  return &Profile;
}
