/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef RUYI_PLATFORM_CPU_INFO_LIB_H
#define RUYI_PLATFORM_CPU_INFO_LIB_H
#include <Uefi.h>
typedef EFI_STATUS (*INFO_EMIT)(
  VOID          *Context,
  CONST CHAR16  *Label,
  CONST CHAR16  *Value
  );
EFI_STATUS
PlatformCpuInfo (
  INFO_EMIT  Emit,
  VOID       *Context
  );

#endif
