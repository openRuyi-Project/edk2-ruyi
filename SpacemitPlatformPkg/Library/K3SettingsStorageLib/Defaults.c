/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IndustryStandard/K3FirmwareDefaults.h>
#include <Library/PlatformSettingsLib.h>
#include <Library/K3SpiNorLib.h>

STATIC EFI_STATUS
Access (UINTN Slot, UINTN Offset, UINTN Size, VOID *Data, BOOLEAN Write, BOOLEAN Erase)
{
  UINT32 Address;
  EFI_STATUS Status;
  if (Slot >= SETTINGS_SLOT_COUNT || Offset > K3_DEFAULTS_STRIDE ||
      Size > K3_DEFAULTS_STRIDE - Offset || (Size && Data == NULL)) { return EFI_INVALID_PARAMETER; }
  Address = K3_DEFAULTS_OFFSET + (UINT32)Slot * K3_DEFAULTS_STRIDE + (UINT32)Offset;
  EFI_SPI_NOR_FLASH_PROTOCOL *Nor;
  Status = K3SpiNorFindFlash (&Nor);
  if (EFI_ERROR (Status)) { return Status; }
  if (Nor->FlashSize != SIZE_8MB || Nor->EraseBlockBytes != SIZE_4KB) { return EFI_UNSUPPORTED; }
  if (Erase) { return Nor->Erase (Nor, Address, 1); }
  return Write ? Nor->WriteData (Nor, Address, Size, Data) : Nor->ReadData (Nor, Address, Size, Data);
}
EFI_STATUS PlatformSettingsStorageRead (UINTN Slot, UINTN Offset, UINTN Size, VOID *Data)
{ return Access (Slot, Offset, Size, Data, FALSE, FALSE); }
EFI_STATUS PlatformSettingsStorageWrite (UINTN Slot, UINTN Offset, UINTN Size, CONST VOID *Data)
{ return Access (Slot, Offset, Size, (VOID *)Data, TRUE, FALSE); }
EFI_STATUS PlatformSettingsStoragePrepare (UINTN Slot)
{ return Access (Slot, 0, 0, NULL, FALSE, TRUE); }
