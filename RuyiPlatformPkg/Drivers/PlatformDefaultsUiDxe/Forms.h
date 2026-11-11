/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef PLATFORM_FORMS_H_
#define PLATFORM_FORMS_H_
#define CPU_QUESTION_ID 0x1002
#define CPU_LABEL 0x2000
#define CPU_LABEL_END 0x2001
#define PLATFORM_FORMSET_GUID \
  { 0x4a01a428, 0xbf2e, 0x44f5, { 0x9f, 0x70, 0xe9, 0x5e, 0xec, 0x8a, 0x03, 0x1b } }
typedef struct {
  UINT8    EmulationEnabled;
  UINT8    Available;
  UINT8    PxeBoot;
  UINT8    HttpBoot;
  UINT8    EccAvailable;
  UINT8    EccEnabled;
  UINT8    CpuMode;
  UINT8    CpuAvailable;
  UINT8    OverridesPresent;
} PLATFORM_CONFIGURATION;
#endif
