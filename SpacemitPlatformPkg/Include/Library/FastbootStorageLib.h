// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include <Protocol/BlockIo.h>

typedef enum {
  FastbootStorageErase,
  FastbootStorageWrite,
  FastbootStorageVerify,
  FastbootStorageRead
} FASTBOOT_STORAGE_PHASE;

typedef EFI_STATUS (*FASTBOOT_STORAGE_PROGRESS) (
  UINT64                  Done,
  UINT64                  Total,
  FASTBOOT_STORAGE_PHASE  Phase
  );

// Sparse offsets are relative to the whole selected block device.
// DONT_CARE preserves data, including data from earlier downloads.
// Keep one media ID through writing and verification; a change aborts the transfer.
EFI_STATUS
FastbootWriteImage (
  EFI_BLOCK_IO_PROTOCOL      *Io,
  CONST VOID                 *Data,
  UINTN                      Size,
  BOOLEAN                    AllowSparse,
  FASTBOOT_STORAGE_PROGRESS  Progress
  );

typedef struct {
  CHAR8                    Name[32];
  EFI_BLOCK_IO_PROTOCOL    *Io;
  UINTN                    Segment, Bus, Device, Function;
  UINT32                   Namespace;
} FASTBOOT_NVME_TARGET;

EFI_STATUS
FastbootLocateNvme (
  FASTBOOT_NVME_TARGET  **Targets,
  UINTN                 *Count
  );
