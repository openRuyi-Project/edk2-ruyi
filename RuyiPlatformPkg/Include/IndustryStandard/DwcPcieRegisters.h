/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Base.h>

// DesignWare DBI and outbound unrolled iATU register layout.
#define DWC_DBI_MISC_CONTROL       0x8bc
#define DWC_DBI_RO_WRITE_ENABLE    BIT0
#define DWC_ATU_REGION_STRIDE      0x200
#define DWC_ATU_CONTROL1           0x00
#define DWC_ATU_CONTROL2           0x04
#define DWC_ATU_LOWER_BASE         0x08
#define DWC_ATU_UPPER_BASE         0x0c
#define DWC_ATU_LIMIT              0x10
#define DWC_ATU_LOWER_TARGET       0x14
#define DWC_ATU_UPPER_TARGET       0x18
#define DWC_ATU_UPPER_LIMIT        0x20
#define DWC_ATU_ENABLE             BIT31
#define DWC_ATU_INCREASE_REGION    BIT13
#define DWC_VERSION_460A           0x3436302a
