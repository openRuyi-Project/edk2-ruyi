// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/Eic7700BoardProfileLib.h>

CONST EIC7700_BOARD_PROFILE *
Eic7700GetBoardProfile (VOID)
{
  STATIC CONST EIC7700_BOARD_PROFILE Profile = {
    {"sifive,hifive-premier-p550", "CONFIG_BOARD_SIFIVE_HIFIVE_PREMIER_P550=y", NULL, 0x82200000, SIZE_4MB},
    "SiFive", 0x90000000, 0, 0, TRUE
  };
  return &Profile;
}
