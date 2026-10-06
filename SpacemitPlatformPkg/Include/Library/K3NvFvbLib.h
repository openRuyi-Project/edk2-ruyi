/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_NV_FVB_LIB_H
#define K3_NV_FVB_LIB_H
#include <Uefi.h>
#include <Pi/PiFirmwareVolume.h>
#include <Protocol/FirmwareVolumeBlock.h>

#define K3_NV_FLASH_OFFSET   0x700000U
#define K3_NV_VARIABLE_SIZE  0x40000U
#define K3_NV_WORKING_SIZE   0x10000U
#define K3_NV_SPARE_SIZE     0x40000U
#define K3_NV_TOTAL_SIZE     0x90000U
#define K3_NV_BLOCK_SIZE     0x1000U

typedef EFI_STATUS (*K3_NV_READ)(
  VOID    *Context,
  UINT32  Offset,
  UINTN   Bytes,
  VOID    *Buffer
  );
typedef EFI_STATUS (*K3_NV_PROGRAM)(
  VOID        *Context,
  UINT32      Offset,
  UINTN       Bytes,
  CONST VOID  *Buffer
  );
typedef EFI_STATUS (*K3_NV_ERASE)(
  VOID    *Context,
  UINT32  Offset
  );

typedef struct {
  K3_NV_READ       Read;
  K3_NV_PROGRAM    Program;
  K3_NV_ERASE      Erase;
  VOID             *Context;
} K3_NV_FLASH_OPS;

typedef struct {
  UINT32                                 Signature;
  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL    Protocol;
  K3_NV_FLASH_OPS                        Flash;
  UINT8                                  *Mirror;
  EFI_PHYSICAL_ADDRESS                   Physical;
  EFI_FVB_ATTRIBUTES_2                   Attributes;
  BOOLEAN                                Busy;
  BOOLEAN                                Faulted;
  BOOLEAN                                WriteLocked;
  UINT8                                  Scratch[K3_NV_BLOCK_SIZE];
  UINT8                                  Expected[K3_NV_BLOCK_SIZE];
} K3_NV_FVB;

/* Mirror and context must be runtime allocations. Initialization reads
   the reserved region; only an entirely erased region is formatted. */
EFI_STATUS
K3NvFvbInitialize (
  K3_NV_FVB              *Fvb,
  VOID                   *Mirror,
  CONST K3_NV_FLASH_OPS  *Flash
  );

#endif
