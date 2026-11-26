/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Library/FirmwareUpdateLib.h>
#include <Library/Eic7700SpiNorLib.h>

// Boot-time access to the die 1 controller from its own hart.
// These entry points belong to the ESWIN PlatformFirmwareUpdateLib instance.
typedef struct {
  EIC7700_SPI_NOR  Nor;
  VOID             *Channel;
  VOID             *Mapping;
  UINTN            Hart;
  BOOLEAN          Started;
} D560_REMOTE;

EFI_STATUS D560StartRemote (D560_REMOTE *Remote);
EFI_STATUS D560StopRemote (D560_REMOTE *Remote);

EFI_STATUS
D560SecondaryFlashSize (UINT32 *Bytes);

EFI_STATUS
D560SecondaryFlashRead (
  UINT64 Offset, UINTN Size, VOID *Data,
  FIRMWARE_UPDATE_PROGRESS Progress OPTIONAL, VOID *Context OPTIONAL
  );
