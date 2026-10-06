/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <IndustryStandard/SpiNorFlashJedecSfdp.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/K3QspiLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiRuntimeLib.h>
#include "Flash.h"

#define PLL1_SWCR1      0x100
#define PLL1_SWCR2      0x104
#define PLL1_SWCR3      0x108
#define PLL1_DX_ENABLE  BIT22
#define PLL1_DX_DIV     (31U << 23)
#define QSPI_CLK        0x60
#define QSPI_RATE_MASK  (0x3fU << 6)
#define QSPI_RATE       ((5U << 6) | (2U << 9))
#define QSPI_ENABLE     (BIT0 | BIT1 | BIT3 | BIT4)

STATIC K3_QSPI  mQspi = {
  .Signature = K3_QSPI_SIGNATURE,
  .Base      = 0xd420c000,
  .Window    = 0xb8000000,
  .ClockHz   = 25600000
};
STATIC UINTN    mApmu = 0xd4282800;
STATIC UINTN    mApbs = 0xd4090000;
STATIC UINT32   mPageSize;
STATIC UINT32   mProgramUs;
STATIC UINT64   mEraseUs;
STATIC UINT8    mEraseOpcode;

STATIC EFI_STATUS
Prepare (
  VOID
  )
{
  if (mQspi.Faulted) {
    return EFI_DEVICE_ERROR;
  }

  UINT32  Pll   = MmioRead32 (mApbs + PLL1_SWCR2);
  UINT32  Clock = MmioRead32 (mApmu + QSPI_CLK);

  // QSPI uses PLL1 / 32 / 3. Restore only gates Linux may have disabled;
  // never retune a shared PLL or silently use an altered clock source.
  if ((MmioRead32 (mApbs + PLL1_SWCR1) != 0x0b330ccc) ||
      (MmioRead32 (mApbs + PLL1_SWCR3) != 0xa0558989) ||
      ((Pll & (PLL1_DX_DIV | BIT16 | 0xff00)) != (PLL1_DX_DIV | BIT16 | 0xcd00)) ||
      ((Clock & (QSPI_RATE_MASK | BIT12)) != QSPI_RATE))
  {
    return EFI_UNSUPPORTED;
  }

  if (!(Pll & PLL1_DX_ENABLE) || ((Clock & QSPI_ENABLE) != QSPI_ENABLE)) {
    MmioWrite32 (mApbs + PLL1_SWCR2, Pll | PLL1_DX_ENABLE);
    MmioWrite32 (mApmu + QSPI_CLK, Clock | QSPI_ENABLE);
    mQspi.Ready = FALSE;
  }

  return mQspi.Ready ? EFI_SUCCESS : K3QspiControllerInitialize (&mQspi);
}

STATIC EFI_STATUS
Command (
  UINTN  TxBytes,
  UINT8  *Tx,
  UINTN  RxBytes,
  VOID   *Rx
  )
{
  return K3QspiCoreTransaction (
           &mQspi,
           RxBytes ? SPI_TRANSACTION_WRITE_THEN_READ : SPI_TRANSACTION_WRITE_ONLY,
           FALSE,
           0,
           1,
           8,
           (UINT32)TxBytes,
           Tx,
           (UINT32)RxBytes,
           Rx
           );
}

STATIC VOID
Address (
  UINT8   *Tx,
  UINT8   Opcode,
  UINT32  Offset
  )
{
  Tx[0] = Opcode;
  Tx[1] = (UINT8)(Offset >> 16);
  Tx[2] = (UINT8)(Offset >> 8);
  Tx[3] = (UINT8)Offset;
}

STATIC EFI_STATUS
WaitReady (
  UINT64  TimeoutUs
  )
{
  UINT8   Tx = 0x05, StatusByte;
  UINT64  Start = GetPerformanceCounter ();

  do {
    EFI_STATUS  Status = Command (1, &Tx, 1, &StatusByte);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (!(StatusByte & BIT0)) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  } while (GetTimeInNanoSecond (GetPerformanceCounter () - Start) < TimeoutUs * 1000);

  // Do not issue another command against a flash still executing a write.
  mQspi.Faulted = TRUE;
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
WriteEnable (
  VOID
  )
{
  UINT8       Tx = 0x06, StatusByte;
  EFI_STATUS  Status = Command (1, &Tx, 0, NULL);

  if (!EFI_ERROR (Status)) {
    Tx     = 0x05;
    Status = Command (1, &Tx, 1, &StatusByte);
    if (!EFI_ERROR (Status) && ((StatusByte & (BIT0 | BIT1)) != BIT1)) {
      Status = EFI_WRITE_PROTECTED;
    }
  }

  return Status;
}

STATIC BOOLEAN
InStore (
  UINT32  Offset,
  UINTN   Bytes
  )
{
  return Offset >= K3_NV_FLASH_OFFSET &&
         Offset < K3_NV_FLASH_OFFSET + K3_NV_TOTAL_SIZE &&
         Bytes <= K3_NV_FLASH_OFFSET + K3_NV_TOTAL_SIZE - Offset;
}

STATIC EFI_STATUS
ReadFlash (
  VOID    *Context,
  UINT32  Offset,
  UINTN   Bytes,
  VOID    *Buffer
  )
{
  UINT8  Tx[4];

  if (!Buffer || !InStore (Offset, Bytes)) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_STATUS  Status = Prepare ();

  while (!EFI_ERROR (Status) && Bytes) {
    UINTN  Chunk = MIN (Bytes, K3_QSPI_DATA_BYTES);
    Address (Tx, 0x03, Offset);
    Status  = Command (sizeof (Tx), Tx, Chunk, Buffer);
    Offset += Chunk;
    Buffer  = (UINT8 *)Buffer + Chunk;
    Bytes  -= Chunk;
  }

  return Status;
}

STATIC EFI_STATUS
ProgramFlash (
  VOID        *Context,
  UINT32      Offset,
  UINTN       Bytes,
  CONST VOID  *Buffer
  )
{
  UINT8  Tx[4 + K3_QSPI_DATA_BYTES];

  if (!Buffer || !InStore (Offset, Bytes)) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_STATUS  Status = Prepare ();

  while (!EFI_ERROR (Status) && Bytes) {
    UINTN  Chunk = MIN (Bytes, MIN (K3_QSPI_DATA_BYTES, mPageSize - Offset % mPageSize));
    Status = WriteEnable ();
    if (EFI_ERROR (Status)) {
      break;
    }

    Address (Tx, 0x02, Offset);
    CopyMem (Tx + 4, Buffer, Chunk);
    Status = Command (4 + Chunk, Tx, 0, NULL);
    if (!EFI_ERROR (Status)) {
      Status = WaitReady (mProgramUs);
    }

    Offset += Chunk;
    Buffer  = (CONST UINT8 *)Buffer + Chunk;
    Bytes  -= Chunk;
  }

  return Status;
}

STATIC EFI_STATUS
EraseFlash (
  VOID    *Context,
  UINT32  Offset
  )
{
  UINT8  Tx[4];

  if ((Offset % K3_NV_BLOCK_SIZE) || !InStore (Offset, K3_NV_BLOCK_SIZE)) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_STATUS  Status = Prepare ();

  if (!EFI_ERROR (Status)) {
    Status = WriteEnable ();
  }

  if (!EFI_ERROR (Status)) {
    Address (Tx, mEraseOpcode, Offset);
    Status = Command (sizeof (Tx), Tx, 0, NULL);
  }

  return EFI_ERROR (Status) ? Status : WaitReady (mEraseUs);
}

STATIC EFI_STATUS
ReadSfdp (
  UINT32  Offset,
  UINTN   Bytes,
  VOID    *Buffer
  )
{
  UINT8  Tx[5];

  Address (Tx, 0x5a, Offset);
  Tx[4] = 0;
  return Command (sizeof (Tx), Tx, Bytes, Buffer);
}

STATIC EFI_STATUS
Discover (
  VOID
  )
{
  SFDP_HEADER            Header;
  SFDP_PARAMETER_HEADER  Param, Basic = { 0 };
  UINT32                 Table[11];
  EFI_STATUS             Status = ReadSfdp (0, sizeof (Header), &Header);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Header.Signature != SFDP_HEADER_SIGNATURE) || (Header.MajorRev != 1)) {
    return EFI_UNSUPPORTED;
  }

  for (UINTN I = 0; I <= Header.NumParameterHeaders; I++) {
    Status = ReadSfdp (8 + I * 8, sizeof (Param), &Param);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Param.IdLsb == 0) && (Param.IdMsb == 0xff) && (Param.MajorRev == 1) &&
        (Param.Length >= ARRAY_SIZE (Table)) && (Param.MinorRev >= Basic.MinorRev))
    {
      Basic = Param;
    }
  }

  if (!Basic.Length) {
    return EFI_UNSUPPORTED;
  }

  Status = ReadSfdp (Basic.TablePointer, sizeof (Table), Table);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Require uniform 4 KiB erase and three-byte commands supported by the IP.
  if (((Table[0] & 3) != 1) || (((Table[0] >> 17) & 3) > 1) ||
      (((Table[0] >> 8) & 0xff) != 0x20))
  {
    return EFI_UNSUPPORTED;
  }

  mPageSize = 1U << ((Table[10] >> 4) & 15);
  if (mPageSize > 256) {
    return EFI_UNSUPPORTED;
  }

  mProgramUs = 2 * ((Table[10] & 15) + 1) *
               (((Table[10] >> 8) & 31) + 1) * ((Table[10] & BIT13) ? 64 : 8);
  for (UINTN I = 0; I < 4; I++) {
    UINT32  Erase = (Table[7 + I / 2] >> ((I % 2) * 16)) & 0xffff;
    if (Erase == ((0x20 << 8) | 12)) {
      STATIC CONST UINT32  Units[] = { 1000, 16000, 128000, 1000000 };
      UINT32               Timing  = (Table[9] >> (4 + 7 * I)) & 0x7f;
      mEraseOpcode = 0x20;
      mEraseUs     = 2ULL * ((Table[9] & 15) + 1) * ((Timing & 31) + 1) * Units[Timing >> 5];
      return EFI_SUCCESS;
    }
  }

  return EFI_UNSUPPORTED;
}

STATIC EFI_STATUS
RuntimeMmio (
  UINTN  Base
  )
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;

  Base &= ~(UINTN)EFI_PAGE_MASK;
  EFI_STATUS  Status = gDS->GetMemorySpaceDescriptor (Base, &Descriptor);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Descriptor.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) {
    return EFI_UNSUPPORTED;
  }

  return gDS->SetMemorySpaceAttributes (Base, EFI_PAGE_SIZE, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
}

EFI_STATUS
K3NvFlashInitialize (
  UINT32           FlashSize,
  K3_NV_FLASH_OPS  *Ops
  )
{
  if ((FlashSize < K3_NV_FLASH_OFFSET + K3_NV_TOTAL_SIZE) || (FlashSize > SIZE_16MB)) {
    return EFI_UNSUPPORTED;
  }

  mQspi.WindowBytes = FlashSize;
  EFI_STATUS  Status = Prepare ();

  if (!EFI_ERROR (Status)) {
    Status = Discover ();
  }

  if (!EFI_ERROR (Status)) {
    Status = RuntimeMmio (mQspi.Base);
  }

  if (!EFI_ERROR (Status)) {
    Status = RuntimeMmio (mApmu);
  }

  if (!EFI_ERROR (Status)) {
    Status = RuntimeMmio (mApbs);
  }

  if (!EFI_ERROR (Status)) {
    *Ops = (K3_NV_FLASH_OPS) {
      ReadFlash, ProgramFlash, EraseFlash, NULL
    };
  }

  return Status;
}

VOID
K3NvFlashVirtualAddressChange (
  VOID
  )
{
  if (EFI_ERROR (EfiConvertPointer (0, (VOID **)&mQspi.Base)) ||
      EFI_ERROR (EfiConvertPointer (0, (VOID **)&mApmu)) ||
      EFI_ERROR (EfiConvertPointer (0, (VOID **)&mApbs)))
  {
    mQspi.Faulted = TRUE;
  }
}
