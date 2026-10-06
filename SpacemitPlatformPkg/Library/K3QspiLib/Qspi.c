/** @file
  K3 polled QSPI IP-FIFO transactions for the standard SPI I/O protocol.

  Register programming is adapted from the BSD SpacemiT STQspiDxe driver.
  Copyright (C) 2016 Marvell International Ltd.
  Copyright (c) 2024-2025, SpacemiT Co., Ltd.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/K3QspiLib.h>

#define MCR            0x000
#define IPCR           0x008
#define SOCCR          0x024
#define SFAR           0x100
#define SFACR          0x104
#define SMPR           0x108
#define RBCT           0x110
#define TBDR           0x154
#define SR             0x15c
#define FR             0x160
#define RSER           0x164
#define SPTRCLR        0x16c
#define SFA1AD         0x180
#define LUTKEY         0x300
#define LCKCR          0x304
#define LUT1           0x320
#define RBDR           0x200
#define IP_TIMEOUT_US  100000
#define FR_DONE        BIT0
#define FR_XIP         BIT1
// TBFF (bit 27) and RBDF (bit 16) report FIFO state, not transfer faults.
#define FR_ERRORS  (BIT4 | BIT6 | BIT7 | BIT11 | BIT12 | BIT13 | BIT14 | BIT15 | BIT17 | BIT23 | BIT26 | BIT31)

typedef struct {
  UINT32     Address;
  UINT32     AddressBytes;
  UINT32     DummyBits;
  UINT32     Bytes;
  UINT8      Opcode;
  BOOLEAN    Read;
  UINT8      *Data;
} COMMAND;

STATIC UINT32
Read (
  K3_QSPI  *Q,
  UINT32   Offset
  )
{
  return MmioRead32 (Q->Base + Offset);
}

STATIC VOID
Write (
  K3_QSPI  *Q,
  UINT32   Offset,
  UINT32   Value
  )
{
  MmioWrite32 (Q->Base + Offset, Value);
}

STATIC EFI_STATUS
Idle (
  K3_QSPI  *Q
  )
{
  UINT64  Start = GetPerformanceCounter ();

  do {
    if (Read (Q, FR) & FR_XIP) {
      return EFI_ACCESS_DENIED;
    }

    if (!(Read (Q, SR) & 7)) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  } while (GetTimeInNanoSecond (GetPerformanceCounter () - Start) < IP_TIMEOUT_US * 1000ULL);

  return EFI_TIMEOUT;
}

STATIC VOID
ResetBuffers (
  K3_QSPI  *Q
  )
{
  UINT32  Value = Read (Q, MCR);

  Write (Q, MCR, Value | 3);
  MicroSecondDelay (1); // At least one AHB and two serial clock periods.
  Write (Q, MCR, Value & ~3U);
}

STATIC EFI_STATUS
WriteAddressRegister (
  K3_QSPI  *Q,
  UINT32   Offset,
  UINT32   Value
  )
{
  UINT64  Start = GetPerformanceCounter ();

  do {
    Write (Q, Offset, Value);
    if (!(Read (Q, FR) & BIT6)) {
      return EFI_SUCCESS;
    }

    Write (Q, FR, BIT6);
    MicroSecondDelay (1);
  } while (GetTimeInNanoSecond (GetPerformanceCounter () - Start) < IP_TIMEOUT_US * 1000ULL);

  return EFI_TIMEOUT;
}

EFI_STATUS
K3QspiControllerInitialize (
  K3_QSPI  *Q
  )
{
  EFI_STATUS  Status;

  if (!Q || (Q->Signature != K3_QSPI_SIGNATURE) || !Q->Base ||
      !Q->WindowBytes || (Q->Window > MAX_UINT32 - Q->WindowBytes) ||
      !Q->ClockHz || (Q->ClockHz >= 104000000))
  {
    return EFI_INVALID_PARAMETER;
  }

  Q->Ready = FALSE;
  Status   = Idle (Q);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ResetBuffers (Q);
  Write (Q, MCR, Read (Q, MCR) | BIT14);
  Write (Q, SMPR, 0);
  Write (Q, SOCCR, 8);
  Status = WriteAddressRegister (Q, SFAR, Q->Window);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Write (Q, SFACR, 0);
  // Only A1/CS0 is exposed; equal following boundaries disable other CS.
  for (UINT32 Offset = SFA1AD; Offset <= SFA1AD + 12; Offset += 4) {
    Write (Q, Offset, Q->Window + Q->WindowBytes);
  }

  Write (Q, MCR, (Read (Q, MCR) & ~BIT14) | 0x000f000c);
  Status = WriteAddressRegister (Q, RBCT, BIT8);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Write (Q, RSER, 0);
  Write (Q, FR, MAX_UINT32);
  if (((Read (Q, MCR) & (BIT14 | 0x000f000c)) != 0x000f000c) ||
      !(Read (Q, RBCT) & BIT8) || (Read (Q, SFA1AD) != Q->Window + Q->WindowBytes))
  {
    return EFI_DEVICE_ERROR;
  }

  Q->Ready   = TRUE;
  Q->Faulted = FALSE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Decode (
  EFI_SPI_TRANSACTION_TYPE  Type,
  UINT32                    TxBytes,
  UINT8                     *Tx,
  UINT32                    RxBytes,
  UINT8                     *Rx,
  COMMAND                   *C
  )
{
  if (!Tx || !TxBytes || (RxBytes && !Rx)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Type != SPI_TRANSACTION_WRITE_ONLY) && (Type != SPI_TRANSACTION_WRITE_THEN_READ)) {
    return EFI_UNSUPPORTED;
  }

  if (((Type == SPI_TRANSACTION_WRITE_ONLY) && RxBytes) ||
      ((Type == SPI_TRANSACTION_WRITE_THEN_READ) && !RxBytes))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (C, sizeof (*C));
  C->Opcode = Tx[0];
  switch (C->Opcode) {
    case 0x03: // Read array.
      C->AddressBytes = 3;
      C->Read         = TRUE;
      break;
    case 0x0b: // Fast read, one dummy byte.
    case 0x5a: // SFDP, one dummy byte.
      C->AddressBytes = 3;
      C->DummyBits    = 8;
      C->Read         = TRUE;
      break;
    case 0x9f: // JEDEC ID and status registers.
    case 0x05:
    case 0x35:
    case 0x15:
      C->Read = TRUE;
      break;
    case 0x02: // Page program.
    case 0x20: // Sector/block erase.
    case 0x52:
    case 0xd8:
      C->AddressBytes = 3;
      break;
    case 0x06: // Write enable/disable.
    case 0x04:
    case 0x01: // Status register writes.
    case 0x31:
    case 0x11:
      break;
    default:
      return EFI_UNSUPPORTED;
  }

  UINT32  Header = 1 + C->AddressBytes + C->DummyBits / 8;

  if ((TxBytes < Header) || (C->Read != (Type == SPI_TRANSACTION_WRITE_THEN_READ)) ||
      (C->Read && (TxBytes != Header)))
  {
    return EFI_INVALID_PARAMETER;
  }

  for (UINT32 I = 0; I < C->AddressBytes; I++) {
    C->Address = (C->Address << 8) | Tx[I + 1];
  }

  C->Bytes = C->Read ? RxBytes : TxBytes - Header;
  C->Data  = C->Read ? Rx : Tx + Header;
  if (C->Bytes > K3_QSPI_DATA_BYTES) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if (!C->Read) {
    if (C->Opcode == 0x02) {
      if (!C->Bytes || (C->Bytes > 256 - (C->Address & 255))) {
        return EFI_BAD_BUFFER_SIZE;
      }
    } else if ((C->Opcode == 0x01) || (C->Opcode == 0x31) || (C->Opcode == 0x11)) {
      if (!C->Bytes || (C->Bytes > 2)) {
        return EFI_BAD_BUFFER_SIZE;
      }
    } else if (C->Bytes) {
      return EFI_BAD_BUFFER_SIZE;
    }
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Execute (
  K3_QSPI        *Q,
  CONST COMMAND  *C
  )
{
  EFI_STATUS  Status = Idle (Q);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Write (Q, MCR, Read (Q, MCR) | BIT10 | BIT11);
  Write (Q, SPTRCLR, Read (Q, SPTRCLR) | BIT0 | BIT8);
  Write (Q, FR, MAX_UINT32);
  Status = WriteAddressRegister (Q, SFAR, Q->Window + C->Address);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  UINT16  Instructions[8] = { 0 };
  UINTN   Count           = 0;

  Instructions[Count++] = (1U << 10) | C->Opcode;
  if (C->AddressBytes) {
    Instructions[Count++] = (2U << 10) | (C->AddressBytes * 8);
  }

  if (C->DummyBits) {
    Instructions[Count++] = (3U << 10) | C->DummyBits;
  }

  if (C->Bytes) {
    Instructions[Count++] = (C->Read ? 7U : 8U) << 10;
  }

  Write (Q, LUTKEY, 0x5af05af0);
  Write (Q, LCKCR, 2);
  for (UINTN I = 0; I < 4; I++) {
    Write (Q, LUT1 + I * 4, Instructions[I * 2] | ((UINT32)Instructions[I * 2 + 1] << 16));
  }

  Write (Q, LUTKEY, 0x5af05af0);
  Write (Q, LCKCR, 1);
  if (!C->Read && C->Bytes) {
    // Each hardware FIFO pop consumes 128 bits, including the last pop.
    UINT32  FifoBytes = ALIGN_VALUE (C->Bytes, 16);
    for (UINT32 I = 0; I < FifoBytes; I += 4) {
      UINT32  Word = 0;
      if (I < C->Bytes) {
        CopyMem (&Word, C->Data + I, MIN (4U, C->Bytes - I));
      }

      Write (Q, TBDR, Word);
    }
  }

  MemoryFence ();
  Write (Q, FR, MAX_UINT32);
  Write (Q, IPCR, BIT24 | C->Bytes);
  UINT64  Start = GetPerformanceCounter ();

  do {
    UINT32  Flags = Read (Q, FR);
    if (Flags & FR_ERRORS) {
      return EFI_DEVICE_ERROR;
    }

    if (Flags & FR_DONE) {
      Status = Idle (Q);
      if (EFI_ERROR (Status) || (Read (Q, FR) & FR_ERRORS)) {
        return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
      }

      if (C->Read) {
        for (UINT32 I = 0; I < C->Bytes; I += 4) {
          UINT32  Word = Read (Q, RBDR + I);
          CopyMem (C->Data + I, &Word, MIN (4U, C->Bytes - I));
        }
      }

      ResetBuffers (Q);
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  } while (GetTimeInNanoSecond (GetPerformanceCounter () - Start) < IP_TIMEOUT_US * 1000ULL);

  return EFI_TIMEOUT;
}

EFI_STATUS EFIAPI
K3QspiCoreTransaction (
  K3_QSPI                   *Q,
  EFI_SPI_TRANSACTION_TYPE  Type,
  BOOLEAN                   DebugTransaction,
  UINT32                    ClockHz,
  UINT32                    BusWidth,
  UINT32                    FrameSize,
  UINT32                    WriteBytes,
  UINT8                     *WriteBuffer,
  UINT32                    ReadBytes,
  UINT8                     *ReadBuffer
  )
{
  if (!Q || (Q->Signature != K3_QSPI_SIGNATURE)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!Q->Ready || Q->Faulted || Q->Busy) {
    return EFI_NOT_READY;
  }

  if ((BusWidth != 1) || (FrameSize != 8) || (ClockHz && (ClockHz < Q->ClockHz))) {
    return EFI_UNSUPPORTED;
  }

  COMMAND     C;
  EFI_STATUS  Status = Decode (Type, WriteBytes, WriteBuffer, ReadBytes, ReadBuffer, &C);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((C.Address >= Q->WindowBytes) || (C.Bytes > Q->WindowBytes - C.Address)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Q->Busy = TRUE;
  Status  = Execute (Q, &C);
  if (EFI_ERROR (Status)) {
    Q->Faulted = TRUE;
  }

  Q->Busy = FALSE;
  return Status;
}
