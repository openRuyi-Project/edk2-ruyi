/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_SYSTEM_INFO_H_
#define K3_SYSTEM_INFO_H_

#define K3_INFO_FORMSET_GUID \
  { 0x38dbfb20, 0x3596, 0x46e1, { 0x98, 0xdd, 0x76, 0x4b, 0xdb, 0xd1, 0xbc, 0xf4 } }
#define INFO_MAIN      1
#define INFO_CPU       2
#define INFO_MEMORY    3
#define INFO_PCI       4
#define INFO_STORAGE   5
#define INFO_END       0xffff
#define INFO_REFRESH   0x100
#define INFO_PREVIOUS  0x200
#define INFO_NEXT      0x300

#ifndef VFRCOMPILE
  #include <Uefi.h>

typedef EFI_STATUS (*INFO_EMIT)(
  VOID          *Context,
  CONST CHAR16  *Label,
  CONST CHAR16  *Value
  );

EFI_STATUS
CollectSystemInfo (
  UINT16     Form,
  INFO_EMIT  Emit,
  VOID       *Context
  );

#endif // VFRCOMPILE
#endif
