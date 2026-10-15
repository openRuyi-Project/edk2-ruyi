/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_PLATFORM_FORMS_H_
#define K3_PLATFORM_FORMS_H_
#define K3_PLATFORM_FORMSET_GUID \
  { 0x5ff2de72, 0x88c3, 0x4756, { 0xb6, 0xd9, 0xdb, 0xd3, 0x98, 0x33, 0x6b, 0xad } }

typedef struct {
  UINT8    CpuMode;
  UINT8    EmulationEnabled;
} K3_PLATFORM_CONFIGURATION;
#endif
