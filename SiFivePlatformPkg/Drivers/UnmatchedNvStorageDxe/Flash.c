/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/Fu740SpiNorLib.h>
#include "Flash.h"

STATIC BOOLEAN
Range (
  UINT32  Offset,
  UINTN   Size
  )
{
  return Offset >= UNMATCHED_NV_FLASH_OFFSET &&
         Offset < UNMATCHED_NV_FLASH_OFFSET + NV_TOTAL_SIZE &&
         Size <= UNMATCHED_NV_FLASH_OFFSET + NV_TOTAL_SIZE - Offset;
}

STATIC EFI_STATUS
Read (
  VOID    *Context,
  UINT32  Offset,
  UINTN   Bytes,
  VOID    *Buffer
  )
{
  if (!Range (Offset, Bytes)) {
    return EFI_INVALID_PARAMETER;
  }

  return Fu740SpiNorRead (Offset, Bytes, Buffer);
}

STATIC EFI_STATUS
Program (
  VOID        *Context,
  UINT32      Offset,
  UINTN       Bytes,
  CONST VOID  *Buffer
  )
{
  if (!Buffer || !Bytes || !Range (Offset, Bytes)) {
    return EFI_INVALID_PARAMETER;
  }

  CONST UINT8  *Data = Buffer;

  while (Bytes) {
    UINTN       Chunk  = MIN (Bytes, 256 - (Offset & 255));
    EFI_STATUS  Status = Fu740SpiNorWrite (Offset, Chunk, Data);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Offset += Chunk;
    Data   += Chunk;
    Bytes  -= Chunk;
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Erase (
  VOID    *Context,
  UINT32  Offset
  )
{
  if (!Range (Offset, NV_BLOCK_SIZE) || Offset % NV_BLOCK_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  return Fu740SpiNorErase (Offset);
}

EFI_STATUS
UnmatchedNvFlashInitialize (
  NV_FLASH_OPS  *Ops
  )
{
  EFI_STATUS  Status = Fu740SpiNorInit (TRUE);

  if (!EFI_ERROR (Status)) {
    *Ops = (NV_FLASH_OPS) {
      Read, Program, Erase, NULL
    };
  }

  return Status;
}
