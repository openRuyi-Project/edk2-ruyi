/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IndustryStandard/UnmatchedFirmwareDefaults.h>
#include <Library/PlatformSettingsLib.h>
#include <Library/Fu740SpiNorLib.h>

STATIC EFI_STATUS
Access (UINTN Slot, UINTN Offset, UINTN Size, VOID *Data, BOOLEAN Write, BOOLEAN Erase)
{
  UINT32 Address;
  EFI_STATUS Status;
  if (Slot >= SETTINGS_SLOT_COUNT || Offset > UNMATCHED_DEFAULTS_STRIDE ||
      Size > UNMATCHED_DEFAULTS_STRIDE - Offset || (Size && Data == NULL)) { return EFI_INVALID_PARAMETER; }
  Address = UNMATCHED_DEFAULTS_OFFSET + (UINT32)Slot * UNMATCHED_DEFAULTS_STRIDE + (UINT32)Offset;
  Status = Fu740SpiNorInit (FALSE);
  if (EFI_ERROR (Status)) { return Status; }
  if (Erase) { return Fu740SpiNorErase (Address); }
  return Write ? Fu740SpiNorWrite (Address, Size, Data) : Fu740SpiNorRead (Address, Size, Data);
}
EFI_STATUS PlatformSettingsStorageRead (UINTN Slot, UINTN Offset, UINTN Size, VOID *Data)
{ return Access (Slot, Offset, Size, Data, FALSE, FALSE); }
EFI_STATUS PlatformSettingsStorageWrite (UINTN Slot, UINTN Offset, UINTN Size, CONST VOID *Data)
{ return Access (Slot, Offset, Size, (VOID *)Data, TRUE, FALSE); }
EFI_STATUS PlatformSettingsStoragePrepare (UINTN Slot)
{ return Access (Slot, 0, 0, NULL, FALSE, TRUE); }
