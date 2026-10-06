/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "Qspi.h"

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
  )
{
  if (!This) {
    return EFI_INVALID_PARAMETER;
  }

  K3_QSPI  *Q = BASE_CR (This, K3_QSPI, Io);

  if (Q->Signature != K3_QSPI_SIGNATURE) {
    return EFI_INVALID_PARAMETER;
  }

  if (Q->Exited) {
    return EFI_UNSUPPORTED;
  }

  EFI_TPL     OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS  Status = K3QspiCoreTransaction (
                         Q,
                         Type,
                         DebugTransaction,
                         ClockHz,
                         BusWidth,
                         FrameSize,
                         WriteBytes,
                         WriteBuffer,
                         ReadBytes,
                         ReadBuffer
                         );

  gBS->RestoreTPL (OldTpl);
  return Status;
}
