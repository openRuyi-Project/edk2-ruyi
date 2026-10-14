/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_CPU_CONFIG_LIB_H_
#define K3_CPU_CONFIG_LIB_H_

// Last 4 KiB of EARLY_RESERVED; consumed by coreboot before OpenSBI.
#define K3_CPU_CONFIG_OFFSET  0x20f000
#define K3_CPU_CONFIG_SIZE    SIZE_4KB
#define K3_CPU_CONFIG_MAGIC   SIGNATURE_32 ('K', '3', 'C', 'P')

typedef enum {
  K3CpuX100Only,
  K3CpuX100A100,
  K3CpuA100Only
} K3_CPU_MODE;

typedef struct {
  UINT32    Magic;
  UINT16    Version;
  UINT16    Size;
  UINT32    Mode;
  UINT32    Crc32;
} K3_CPU_CONFIG_RECORD;

EFI_STATUS
K3CpuConfigGet (
  UINT32  *Mode
  );

EFI_STATUS
K3CpuConfigSet (
  UINT32  Mode
  );

#endif
