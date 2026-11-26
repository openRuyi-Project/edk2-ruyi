// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/Eic7700BoardProfileLib.h>
#include <Library/FdtBoardInfoLib.h>
#include <Library/FdtLib.h>

STATIC CONST EIC7700_BOARD_PROFILE mBoards[] = {
  {
    .Image = {"sifive,hifive-premier-p550", "CONFIG_BOARD_SIFIVE_HIFIVE_PREMIER_P550=y", NULL, 0x82200000, SIZE_4MB},
    .Manufacturer = "SiFive",
    .FastbootCanSwitchHostPort = TRUE,
    .AllowCpuOppOverride = TRUE
  },
  {
    .Image = {"milkv,megrez", "CONFIG_BOARD_MILKV_MEGREZ=y", NULL, 0x82200000, SIZE_4MB},
    .Manufacturer = "Milk-V",
    .FactoryOffset = 0x780000,
    .FactoryBytes = SIZE_512KB
  },
  {
    .Image = {"pine64,starpro64", "CONFIG_BOARD_PINE64_STARPRO64=y", NULL, 0x82200000, SIZE_4MB},
    .Manufacturer = "PINE64",
    .FactoryOffset = 0xf80000,
    .FactoryBytes = SIZE_512KB
  },
  {
    .Image = { "eswin,eic7702-d560", "CONFIG_BOARD_ESWIN_D560=y", NULL, 0x82200000, SIZE_4MB },
    .Manufacturer = "ESWIN",
    .FactoryOffset = 0xf80000,
    .FactoryBytes = SIZE_512KB,
    .DualDie = TRUE
  }
};

CONST EIC7700_BOARD_PROFILE *
Eic7700GetBoardProfile (VOID)
{
  RUYI_BOARD_INFO Info;
  CONST VOID *Fdt;

  if (EFI_ERROR (FdtBoardInfoGet (&Info, &Fdt))) { return NULL; }
  for (UINTN Index = 0; Index < ARRAY_SIZE (mBoards); Index++) {
    if (FdtStringListSearch (Fdt, 0, "compatible", mBoards[Index].Image.Compatible) >= 0) {
      return &mBoards[Index];
    }
  }
  return NULL;
}
