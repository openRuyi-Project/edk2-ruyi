/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IndustryStandard/SpiNorFlashJedecSfdp.h>
#include <Library/BaseLib.h>
#include <Library/Eic7700SpiNorLib.h>
#include <Library/PremierP550NvFlashLib.h>

STATIC BOOLEAN
Range (UINT32 Offset, UINTN Bytes)
{
  return Bytes && Offset >= P550_NV_OFFSET &&
         Offset < P550_NV_OFFSET + NV_TOTAL_SIZE &&
         Bytes <= P550_NV_OFFSET + NV_TOTAL_SIZE - Offset;
}

STATIC EFI_STATUS
Read (VOID *Context, UINT32 Offset, UINTN Bytes, VOID *Buffer)
{
  return Range (Offset, Bytes) ? Eic7700SpiNorRead (Offset, Bytes, Buffer) : EFI_INVALID_PARAMETER;
}

STATIC EFI_STATUS
Program (VOID *Context, UINT32 Offset, UINTN Bytes, CONST VOID *Buffer)
{
  CONST UINT8 *Data = Buffer;
  UINTN Chunk;
  EFI_STATUS Status;

  if (!Buffer || !Range (Offset, Bytes)) { return EFI_INVALID_PARAMETER; }
  while (Bytes) {
    Chunk = MIN (Bytes, SPI_FLASH_PAGE_SIZE - Offset % SPI_FLASH_PAGE_SIZE);
    Status = Eic7700SpiNorWrite (Offset, Chunk, Data);
    if (EFI_ERROR (Status)) { return Status; }
    Offset += Chunk;
    Data += Chunk;
    Bytes -= Chunk;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Erase (VOID *Context, UINT32 Offset)
{
  if (!Range (Offset, NV_BLOCK_SIZE) || Offset % NV_BLOCK_SIZE) {
    return EFI_INVALID_PARAMETER;
  }
  return Eic7700SpiNorErase (Offset, NV_BLOCK_SIZE);
}

EFI_STATUS
PremierP550NvFlashInitialize (BOOLEAN Runtime, NV_FLASH_OPS *Ops)
{
  EFI_STATUS Status;

  if (!Ops) { return EFI_INVALID_PARAMETER; }
  Status = Eic7700SpiNorInit (Runtime);
  if (EFI_ERROR (Status)) { return Status; }
  if (Eic7700SpiNorSize () < P550_NV_OFFSET + NV_TOTAL_SIZE) {
    return EFI_UNSUPPORTED;
  }
  *Ops = (NV_FLASH_OPS) { Read, Program, Erase, NULL };
  return EFI_SUCCESS;
}
