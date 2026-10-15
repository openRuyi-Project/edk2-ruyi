/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef K3_X64_EMULATION_H_
#define K3_X64_EMULATION_H_

#define K3_X64_EMULATION_GUID \
  { 0x30af54d6, 0x71b9, 0x4b94, { 0xb4, 0xdc, 0xee, 0x57, 0x45, 0x11, 0xca, 0x2d } }

typedef struct {
  UINT8    Enabled;
} K3_X64_EMULATION_CONFIGURATION;

#ifndef VFRCOMPILE
extern EFI_GUID  gK3X64EmulationGuid;
#endif
#endif
