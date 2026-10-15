/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>

// coreboot places this inventory in bytes 40..103 of the DDR training header.
// Its CRC covers Version through PartNumber, independently of training state.
#define K3_MEMORY_INFO_MAGIC    SIGNATURE_32 ('K', '3', 'M', 'I')
#define K3_MEMORY_INFO_VERSION  1
#define K3_MEMORY_LPDDR4X       0
#define K3_MEMORY_LPDDR5        1

typedef struct {
  UINT32    Magic;
  UINT32    Crc32;
  UINT32    Version;
  UINT32    Type;
  UINT32    ChipSelects;
  UINT32    DataRate;
  UINT32    DeviceCount;
  UINT32    DeviceSizeMiB;
  CHAR8     PartNumber[32];
} K3_MEMORY_INFO;

STATIC_ASSERT (sizeof (K3_MEMORY_INFO) == 64, "K3 memory inventory layout");
extern EFI_GUID  gSpacemitMemoryInfoGuid;
