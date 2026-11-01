/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef NV_FVB_LIB_H
#define NV_FVB_LIB_H
#include <Uefi.h>
#include <Pi/PiFirmwareVolume.h>
#include <Protocol/FirmwareVolumeBlock.h>

#define NV_VARIABLE_SIZE  0x40000U
#define NV_WORKING_SIZE   0x10000U
#define NV_SPARE_SIZE     0x40000U
#define NV_TOTAL_SIZE     0x90000U
#define NV_BLOCK_SIZE     0x1000U

typedef EFI_STATUS (*NV_READ)(
  VOID    *Context,
  UINT32  Offset,
  UINTN   Bytes,
  VOID    *Buffer
  );
typedef EFI_STATUS (*NV_PROGRAM)(
  VOID        *Context,
  UINT32      Offset,
  UINTN       Bytes,
  CONST VOID  *Buffer
  );
typedef EFI_STATUS (*NV_ERASE)(
  VOID    *Context,
  UINT32  Offset
  );

typedef struct {
  NV_READ       Read;
  NV_PROGRAM    Program;
  NV_ERASE      Erase;
  VOID          *Context;
} NV_FLASH_OPS;

typedef struct {
  UINT32                                 Signature;
  UINT32                                 FlashOffset;
  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL    Protocol;
  NV_FLASH_OPS                           Flash;
  UINT8                                  *Mirror;
  EFI_PHYSICAL_ADDRESS                   Physical;
  EFI_FVB_ATTRIBUTES_2                   Attributes;
  BOOLEAN                                Busy;
  BOOLEAN                                Faulted;
  BOOLEAN                                WriteLocked;
  UINT8                                  Scratch[NV_BLOCK_SIZE];
  UINT8                                  Expected[NV_BLOCK_SIZE];
} NV_FVB;

/* Mirror and context must be runtime allocations. Initialization reads
   the reserved region; only an entirely erased region is formatted. */
EFI_STATUS
NvFvbInitialize (
  NV_FVB              *Fvb,
  VOID                *Mirror,
  CONST NV_FLASH_OPS  *Flash,
  UINT32              FlashOffset
  );

#endif
