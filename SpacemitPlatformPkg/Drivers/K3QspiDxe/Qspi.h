/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/K3QspiLib.h>

EFI_STATUS EFIAPI
K3QspiTransaction (
  CONST EFI_SPI_IO_PROTOCOL  *This,
  EFI_SPI_TRANSACTION_TYPE   Type,
  BOOLEAN                    DebugTransaction,
  UINT32                     ClockHz,
  UINT32                     BusWidth,
  UINT32                     FrameSize,
  UINT32                     WriteBytes,
  UINT8                      *WriteBuffer,
  UINT32                     ReadBytes,
  UINT8                      *ReadBuffer
  );
