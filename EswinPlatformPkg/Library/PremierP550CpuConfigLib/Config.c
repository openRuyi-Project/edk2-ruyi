// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Guid/PremierP550CpuConfig.h>
#include <Library/CpuConfigLib.h>
#include <Library/Eic7700BoardProfileLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#define OPTION_NAME  L"Allow1600MHzOpp"
#define OPTION_ATTR  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS)

EFI_STATUS
CpuConfigGet (CPU_CONFIG *Config)
{
  STATIC CONST CPU_CONFIG_MODE Modes[] = {
    { P550_CPU_STANDARD, L"Board default" },
    { P550_CPU_ALLOW_1600, L"Allow 1.6 GHz in Linux" }
  };
  UINT8 Mode = P550_CPU_STANDARD;
  UINTN Size = sizeof (Mode);
  UINT32 Attributes;
  EFI_STATUS Status;

  if (Config == NULL) { return EFI_INVALID_PARAMETER; }
  CONST EIC7700_BOARD_PROFILE *Board = Eic7700GetBoardProfile ();
  if (!Board || !Board->AllowCpuOppOverride) { return EFI_UNSUPPORTED; }
  Status = gRT->GetVariable (OPTION_NAME, &gPremierP550CpuConfigGuid, &Attributes, &Size, &Mode);
  if (Status == EFI_NOT_FOUND || Status == EFI_BUFFER_TOO_SMALL) {
    Mode = P550_CPU_STANDARD;
  } else if (EFI_ERROR (Status)) {
    return Status;
  } else if (Attributes != OPTION_ATTR || Size != sizeof (Mode) || Mode != P550_CPU_ALLOW_1600) {
    Mode = P550_CPU_STANDARD;
  }
  Config->Default = Mode;
  Config->Modes = Modes;
  Config->Count = ARRAY_SIZE (Modes);
  Config->Help = L"Allow 1.6 GHz only on a P550 modified for a fixed 0.9 V CPU supply. "
                 L"This setting changes the device tree passed to Linux.";
  return EFI_SUCCESS;
}

EFI_STATUS
CpuConfigSet (UINT8 Mode)
{
  EFI_STATUS Status;
  CONST EIC7700_BOARD_PROFILE *Board = Eic7700GetBoardProfile ();
  if (!Board || !Board->AllowCpuOppOverride) { return EFI_UNSUPPORTED; }
  if (Mode == P550_CPU_STANDARD) {
    Status = gRT->SetVariable (OPTION_NAME, &gPremierP550CpuConfigGuid, 0, 0, NULL);
    return Status == EFI_NOT_FOUND ? EFI_SUCCESS : Status;
  }
  if (Mode != P550_CPU_ALLOW_1600) { return EFI_INVALID_PARAMETER; }
  return gRT->SetVariable (OPTION_NAME, &gPremierP550CpuConfigGuid, OPTION_ATTR, sizeof (Mode), &Mode);
}
