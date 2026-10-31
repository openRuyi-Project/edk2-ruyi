/* SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef UNMATCHED_SETTINGS_RECORD_H_
#define UNMATCHED_SETTINGS_RECORD_H_
#include <Uefi.h>

#define SETTINGS_OFFSET           0x10000
#define SETTINGS_SECTOR_SIZE      0x1000
#define SETTINGS_MAGIC            SIGNATURE_32 ('U', 'M', 'C', 'F')
#define SETTINGS_COMMIT           SIGNATURE_32 ('D', 'O', 'N', 'E')
#define SETTINGS_ENABLED          BIT0
#define SETTINGS_DEFAULT_ENABLED  BIT1

typedef struct {
  UINT32    Magic;
  UINT16    Version;
  UINT16    Size;
  UINT32    Generation;
  UINT32    Flags;
  UINT32    Crc32;
  UINT32    Commit;
} SETTINGS_RECORD;

STATIC_ASSERT (sizeof (SETTINGS_RECORD) == 24, "SPI settings layout changed");

EFI_STATUS
SettingsFlashInit (
  VOID
  );

EFI_STATUS
SettingsFlashRead (
  UINT32  Offset,
  UINTN   Size,
  VOID    *Buffer
  );

EFI_STATUS
SettingsFlashWrite (
  UINT32      Offset,
  UINTN       Size,
  CONST VOID  *Buffer
  );

EFI_STATUS
SettingsFlashErase (
  UINT32  Offset
  );

#endif
