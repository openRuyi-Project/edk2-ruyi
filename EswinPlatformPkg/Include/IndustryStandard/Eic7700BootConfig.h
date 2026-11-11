/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef EIC7700_BOOT_CONFIG_H
#define EIC7700_BOOT_CONFIG_H

// Two independent erase blocks hold a CRC32-protected early-boot policy.
// All words are little-endian. The newest valid generation is selected.
#define EIC7700_BOOT_CONFIG_OFFSET       0x700000U
#define EIC7700_BOOT_CONFIG_SLOT_SIZE    0x10000U
#define EIC7700_BOOT_CONFIG_SLOT_COUNT   2U
#define EIC7700_BOOT_CONFIG_BYTES        20U
#define EIC7700_BOOT_CONFIG_MAGIC        0x43424952U
#define EIC7700_BOOT_CONFIG_VERSION      1U
#define EIC7700_BOOT_CONFIG_MAGIC_WORD   0U
#define EIC7700_BOOT_CONFIG_VERSION_WORD 1U
#define EIC7700_BOOT_CONFIG_GEN_WORD     2U
#define EIC7700_BOOT_CONFIG_FLAGS_WORD   3U
#define EIC7700_BOOT_CONFIG_CRC_WORD     4U
#define EIC7700_BOOT_CONFIG_ECC          1U

#endif
