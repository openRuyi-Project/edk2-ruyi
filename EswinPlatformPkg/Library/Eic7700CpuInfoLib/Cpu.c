/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/Eic7700InfoLib.h>
#include <Library/PlatformCpuInfoLib.h>
#include <Library/PrintLib.h>

#define EMIT(Label, Value)  do { Status = Emit (Context, Label, Value); if (EFI_ERROR (Status)) return Status; } while (0)

EFI_STATUS
PlatformCpuInfo (
  INFO_EMIT  Emit,
  VOID       *Context
  )
{
  EIC7700_BOARD_INFO  Info;
  CHAR16            Text[128];
  EFI_STATUS        Status = Eic7700GetBoardInfo (&Info);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  UnicodeSPrint (Text, sizeof (Text), L"%a", Info.Model);
  EMIT (L"System", Text);
  EMIT (L"Processor", L"ESWIN EIC7700");
  UnicodeSPrint (Text, sizeof (Text), L"%u SiFive P550", Info.CoreCount);
  EMIT (L"Enabled cores", Text);
  UnicodeSPrint (Text, sizeof (Text), L"%Lu", Info.BootHart);
  EMIT (L"Boot hart", Text);
  if (Info.CpuRate) {
    UnicodeSPrint (Text, sizeof (Text), L"%Lu MHz", Info.CpuRate / 1000000);
  } else {
    UnicodeSPrint (Text, sizeof (Text), L"Unknown");
  }

  EMIT (L"Core clock", Text);
  return EFI_SUCCESS;
}
