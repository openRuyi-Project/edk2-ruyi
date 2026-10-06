/** SPDX-License-Identifier: BSD-2-Clause-Patent */

#ifndef K3_QSPI_H
#define K3_QSPI_H
#include <Uefi.h>
#include <Protocol/SpiIo.h>

#define K3_QSPI_SIGNATURE   SIGNATURE_32 ('K', '3', 'Q', 'S')
#define K3_QSPI_DATA_BYTES  128

typedef struct {
  UINT32                 Signature;
  UINTN                  Base;
  UINT32                 Window;
  UINT32                 WindowBytes;
  UINT32                 ClockHz;
  BOOLEAN                Ready;
  BOOLEAN                Faulted;
  BOOLEAN                Busy;
  BOOLEAN                Exited;
  EFI_SPI_IO_PROTOCOL    Io;
} K3_QSPI;

EFI_STATUS
K3QspiControllerInitialize (
  K3_QSPI  *Qspi
  );

EFI_STATUS EFIAPI
K3QspiCoreTransaction (
  K3_QSPI                   *Qspi,
  EFI_SPI_TRANSACTION_TYPE  Type,
  BOOLEAN                   DebugTransaction,
  UINT32                    ClockHz,
  UINT32                    BusWidth,
  UINT32                    FrameSize,
  UINT32                    WriteBytes,
  UINT8                     *WriteBuffer,
  UINT32                    ReadBytes,
  UINT8                     *ReadBuffer
  );

#endif
