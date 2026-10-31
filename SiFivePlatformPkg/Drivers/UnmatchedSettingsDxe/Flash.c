/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/Fu740SpiNorLib.h>
#include "Record.h"

EFI_STATUS
SettingsFlashInit (
  VOID
  )
{
  return Fu740SpiNorInit (FALSE);
}

STATIC BOOLEAN
SettingsRange (
  UINT32  Offset,
  UINTN   Size
  )
{
  return Offset >= SETTINGS_OFFSET && Offset < SETTINGS_OFFSET + 2 * SETTINGS_SECTOR_SIZE &&
         Size <= SETTINGS_OFFSET + 2 * SETTINGS_SECTOR_SIZE - Offset;
}

EFI_STATUS
SettingsFlashRead (
  UINT32  Offset,
  UINTN   Size,
  VOID    *Buffer
  )
{
  return Fu740SpiNorRead (Offset, Size, Buffer);
}

EFI_STATUS
SettingsFlashWrite (
  UINT32      Offset,
  UINTN       Size,
  CONST VOID  *Buffer
  )
{
  return SettingsRange (Offset, Size) ? Fu740SpiNorWrite (Offset, Size, Buffer) : EFI_INVALID_PARAMETER;
}

EFI_STATUS
SettingsFlashErase (
  UINT32  Offset
  )
{
  return SettingsRange (Offset, SETTINGS_SECTOR_SIZE) ? Fu740SpiNorErase (Offset) : EFI_INVALID_PARAMETER;
}
