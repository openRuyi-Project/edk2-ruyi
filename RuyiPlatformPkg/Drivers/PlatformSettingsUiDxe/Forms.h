/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef PLATFORM_FORMS_H_
#define PLATFORM_FORMS_H_
#define PLATFORM_FORMSET_GUID \
  { 0x4a01a428, 0xbf2e, 0x44f5, { 0x9f, 0x70, 0xe9, 0x5e, 0xec, 0x8a, 0x03, 0x1b } }
typedef struct {
  UINT8    EmulationEnabled;
  UINT8    Available;
  UINT8    PxeBoot;
  UINT8    HttpBoot;
  UINT8    EccAvailable;
  UINT8    EccEnabled;
} PLATFORM_CONFIGURATION;
#endif
