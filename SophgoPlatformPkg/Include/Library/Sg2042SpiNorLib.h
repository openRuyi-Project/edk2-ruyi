/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>

#define SG2042_FIRMWARE_SPI   0x7002180000ULL
#define SG2042_SPIFMC_GATE    BIT9
#define SG2042_SCP_TABLE      0x600000U
#define SG2042_SCP_MAGIC      0x55aa55aaU
#define SG2042_VARIABLE_BASE  0x700000U

typedef struct {
  UINTN  Base;
  UINTN  Gate;
} SG2042_SPI_NOR;

EFI_STATUS Sg2042SpiReadId (CONST SG2042_SPI_NOR *Nor, UINT8 Id[3]);
EFI_STATUS Sg2042SpiReadUid (CONST SG2042_SPI_NOR *Nor, UINT8 Uid[16]);
EFI_STATUS Sg2042SpiRead (CONST SG2042_SPI_NOR *Nor, UINT32 Offset, UINTN Bytes, VOID *Buffer);
EFI_STATUS Sg2042SpiProgram (CONST SG2042_SPI_NOR *Nor, UINT32 Offset, UINTN Bytes, CONST VOID *Buffer);
EFI_STATUS Sg2042SpiErase (CONST SG2042_SPI_NOR *Nor, UINT32 Offset);
