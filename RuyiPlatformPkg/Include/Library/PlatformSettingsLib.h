/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Protocol/PlatformSettings.h>
#define SETTINGS_RECORD_MAGIC SIGNATURE_32 ('R', 'Y', 'C', 'F')
#define SETTINGS_RECORD_COMMIT SIGNATURE_32 ('D', 'O', 'N', 'E')
#define SETTINGS_SLOT_COUNT 2

typedef struct {
  UINT32 Magic;
  UINT16 Version;
  UINT16 Size;
  UINT32 Generation;
  UINT32 Values;
  UINT32 Crc32;
  UINT32 Commit;
} RUYI_SETTINGS_RECORD;
STATIC_ASSERT (sizeof (RUYI_SETTINGS_RECORD) == 24, "Platform defaults ABI");
BOOLEAN PlatformSettingsRecordValid (CONST RUYI_SETTINGS_RECORD *Record);
INTN PlatformSettingsNewest (CONST RUYI_SETTINGS_RECORD Records[SETTINGS_SLOT_COUNT]);
EFI_STATUS PlatformSettingsImageValid (CONST VOID *Image, UINTN Size, UINT32 Offset, UINT32 Stride);
// Storage adapters may use SPI or block storage such as an eMMC boot area.
// Slots must be independent. Prepare fills an inactive slot with 0xff.
// Partial writes must preserve bytes outside the requested range.
EFI_STATUS PlatformSettingsStorageRead (UINTN Slot, UINTN Offset, UINTN Size, VOID *Data);
EFI_STATUS PlatformSettingsStorageWrite (UINTN Slot, UINTN Offset, UINTN Size, CONST VOID *Data);
EFI_STATUS PlatformSettingsStoragePrepare (UINTN Slot);
EFI_STATUS PlatformSettingsSnapshot (RUYI_SETTINGS_RECORD *Record);
