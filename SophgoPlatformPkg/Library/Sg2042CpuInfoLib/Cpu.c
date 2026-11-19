/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/FdtBoardInfoLib.h>
#include <Library/PlatformCpuInfoLib.h>
#include <Library/PrintLib.h>
#include <Library/Sg2042ClockLib.h>

#define EMIT(Label, Value)  do { Status = Emit (Context, Label, Value); if (EFI_ERROR (Status)) return Status; } while (0)

EFI_STATUS
PlatformCpuInfo (
  INFO_EMIT  Emit,
  VOID       *Context
  )
{
  RUYI_BOARD_INFO  Info;
  CHAR16            Text[128];
  CONST VOID        *Fdt;
  UINT64            Rate;
  EFI_STATUS        Status = FdtBoardInfoGet (&Info, &Fdt);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  UnicodeSPrint (Text, sizeof (Text), L"%a", Info.Model);
  EMIT (L"System", Text);
  EMIT (L"Processor", L"SOPHGO SG2042");
  UnicodeSPrint (Text, sizeof (Text), L"%u C920", Info.CoreCount);
  EMIT (L"Enabled cores", Text);
  UnicodeSPrint (Text, sizeof (Text), L"%Lu", Info.BootHart);
  EMIT (L"Boot hart", Text);
  Rate = Sg2042GetClockRate (Fdt, Sg2042CpuClock);
  if (Rate != 0) {
    UnicodeSPrint (Text, sizeof (Text), L"%Lu MHz", Rate / 1000000);
  } else {
    UnicodeSPrint (Text, sizeof (Text), L"Unknown");
  }

  EMIT (L"Core clock", Text);
  return EFI_SUCCESS;
}
