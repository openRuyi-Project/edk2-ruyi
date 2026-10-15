/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Guid/K3MemoryInfo.h>

#define K3_DDR_TRAINING_ADDRESS  0xc08d0000U
#define K3_DDR_TRAINING_OFFSET   0x10000U
#define K3_DDR_TRAINING_MAGIC    0x54524444U
#define K3_DDR_TRAINING_BYTES    0x9800U

typedef struct {
  UINT32            Magic;
  UINT32            Crc32;
  UINT64            ChipId;
  UINT64            MacAddress;
  UINT32            Version;
  UINT32            Type;
  UINT32            ChipSelects;
  UINT32            DataRate;
  K3_MEMORY_INFO    MemoryInfo;
  UINT8             Reserved[128 - 40 - sizeof (K3_MEMORY_INFO)];
  UINT8             TrainingInfo[K3_DDR_TRAINING_BYTES - 128];
} K3_DDR_TRAINING_RECORD;

STATIC_ASSERT (sizeof (K3_DDR_TRAINING_RECORD) == K3_DDR_TRAINING_BYTES, "DDR record layout");
extern EFI_GUID  gSpacemitDdrTrainingGuid;
