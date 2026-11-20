/** SPDX-License-Identifier: BSD-2-Clause-Patent
    SG2042 SPIFMC register protocol from SOPHGO's SpifmcDxe.
**/
#include <Library/BaseMemoryLib.h>
#include <Library/CpuMmioAddressLib.h>
#include <Library/IoLib.h>
#include <Library/Sg2042SpiNorLib.h>

#define FIFO              0x18
#define FIFO_COUNT        0x20
#define INT_STATUS        0x28
#define TRAN_COUNT        0x14
#define TRAN_CTRL         0x10
#define RX                BIT0
#define TX                BIT1
#define WITH_COMMAND      BIT11
#define FIFO_TRIGGER_8    (3U << 12)
#define GO                BIT15

// Both SPIFMC controllers share the AHB gate. Linux can stop the clock
// after ExitBootServices. Never poll a controller with its clock off.
#define SPIFMC_GATE      BIT9
STATIC UINT64
Ticks (VOID)
{
  UINT64 Value;
  __asm__ volatile ("rdtime %0" : "=r" (Value));
  return Value;
}

STATIC EFI_STATUS
Wait (UINTN Base, UINTN Register, UINT32 Mask, UINT32 Expected)
{
  UINT64 Start = Ticks ();
  do {
    if ((MmioRead32 (Base + Register) & Mask) == Expected) { return EFI_SUCCESS; }
  } while (Ticks () - Start < 5000000); // 100 ms at the SG2042 50 MHz timebase.
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
Command (CONST SG2042_SPI_NOR *Nor, UINT8 Op, BOOLEAN Addressed, UINT32 Address, UINT8 *Buffer, UINTN Bytes, BOOLEAN Input)
{
  UINTN Base = CpuMmioAddress (Nor->Base);
  UINT32 Mode = WITH_COMMAND | GO;
  UINTN Length = Bytes;
  EFI_STATUS Status;
  if (Length > MAX_UINT16 || (!Addressed && Bytes > 3)) { return EFI_BAD_BUFFER_SIZE; }
  if (!(MmioRead32 (CpuMmioAddress (Nor->Gate)) & SPIFMC_GATE)) { return EFI_NOT_READY; }
  // Disable direct memory mapping and let the transfer engine drive chip select.
  MmioWrite32 (Base + 0x0c, 0);
  MmioWrite32 (Base + 0x04, 0);
  MmioWrite32 (Base + FIFO_COUNT, 0);
  MmioWrite8 (Base + FIFO, Op);
  if (Addressed) {
    Mode |= 4U << 8;
    for (INTN Shift = 24; Shift >= 0; Shift -= 8) {
      MmioWrite8 (Base + FIFO, (UINT8)(Address >> Shift));
    }
    // SG2042 has a one-byte read interrupt threshold; writes use eight.
    if (!Input && Bytes) { Mode |= FIFO_TRIGGER_8; }
  } else if (Input) {
    // The controller needs dummy TX bytes for addressless register reads.
    Mode |= TX;
    for (UINTN I = 0; I < Bytes; I++) { MmioWrite8 (Base + FIFO, 0xff); }
  }
  if (Bytes) { Mode |= Input ? RX : TX; }
  MmioWrite32 (Base + INT_STATUS, 0);
  MmioWrite32 (Base + TRAN_COUNT, (UINT32)Length);
  MmioWrite32 (Base + TRAN_CTRL, Mode);
  if (!Addressed && Input) {
    Status = Wait (Base, INT_STATUS, BIT0, BIT0);
    if (EFI_ERROR (Status)) { return Status; }
    for (UINTN I = 0; I < Bytes; I++) { Buffer[I] = MmioRead8 (Base + FIFO); }
  } else if (Bytes) {
    if (Input) {
      Status = Wait (Base, INT_STATUS, BIT2, BIT2);
      if (EFI_ERROR (Status)) { return Status; }
    }
    if (!Input) {
      Status = Wait (Base, FIFO_COUNT, 0xf, 0);
      if (EFI_ERROR (Status)) { return Status; }
      MmioWrite32 (Base + FIFO_COUNT, 0);
    }
    for (UINTN Done = 0; Done < Length;) {
      UINTN Chunk = MIN (Length - Done, 8U);
      Status = Wait (Base, FIFO_COUNT, 0xf, Input ? (UINT32)Chunk : 0);
      if (EFI_ERROR (Status)) { return Status; }
      for (UINTN I = 0; I < Chunk; I++, Done++) {
        if (!Input) { MmioWrite8 (Base + FIFO, Buffer[Done]); }
        else {
          Buffer[Done] = MmioRead8 (Base + FIFO);
        }
      }
    }
  }
  return Wait (Base, INT_STATUS, BIT0, BIT0);
}

EFI_STATUS
Sg2042SpiRead (CONST SG2042_SPI_NOR *Nor, UINT32 Address, UINTN Bytes, VOID *Buffer)
{
  UINT8 *Out = Buffer;
  if (Address > SIZE_64MB || Bytes > SIZE_64MB - Address) { return EFI_INVALID_PARAMETER; }
  while (Bytes) {
    UINTN Chunk = MIN (Bytes, SIZE_4KB);
    EFI_STATUS Status = Command (Nor, 0x13, TRUE, Address, Out, Chunk, TRUE);
    if (EFI_ERROR (Status)) { return Status; }
    Address += (UINT32)Chunk; Out += Chunk; Bytes -= Chunk;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Modify (CONST SG2042_SPI_NOR *Nor, UINT8 Op, UINT32 Address, CONST VOID *Buffer, UINTN Bytes)
{
  UINT8 Sr;
  EFI_STATUS Status = Command (Nor, 0x06, FALSE, 0, NULL, 0, FALSE);
  if (!EFI_ERROR (Status)) { Status = Command (Nor, 0x05, FALSE, 0, &Sr, 1, TRUE); }
  if (EFI_ERROR (Status)) { return Status; }
  if (!(Sr & BIT1)) { return EFI_WRITE_PROTECTED; }
  Status = Command (Nor, Op, TRUE, Address, (VOID *)Buffer, Bytes, FALSE);
  if (EFI_ERROR (Status)) { return Status; }
  UINT64 Start = Ticks ();
  do {
    Status = Command (Nor, 0x05, FALSE, 0, &Sr, 1, TRUE);
    if (EFI_ERROR (Status) || !(Sr & BIT0)) { return Status; }
  } while (Ticks () - Start < 100000000);
  return EFI_TIMEOUT;
}

EFI_STATUS
Sg2042SpiProgram (CONST SG2042_SPI_NOR *Nor, UINT32 Address, UINTN Bytes, CONST VOID *Buffer)
{
  CONST UINT8 *In = Buffer;
  if (Address > SIZE_64MB || Bytes > SIZE_64MB - Address) { return EFI_INVALID_PARAMETER; }
  while (Bytes) {
    UINTN Chunk = MIN (Bytes, 256 - (Address & 255));
    EFI_STATUS Status = Modify (Nor, 0x12, Address, In, Chunk);
    if (EFI_ERROR (Status)) { return Status; }
    Address += (UINT32)Chunk; In += Chunk; Bytes -= Chunk;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
Sg2042SpiErase (CONST SG2042_SPI_NOR *Nor, UINT32 Address)
{
  if ((Address & (SIZE_4KB - 1)) || Address >= SIZE_64MB) { return EFI_INVALID_PARAMETER; }
  return Modify (Nor, 0x21, Address, NULL, 0);
}

EFI_STATUS
Sg2042SpiReadId (CONST SG2042_SPI_NOR *Nor, UINT8 Id[3])
{
  EFI_STATUS Status = Command (Nor, 0x9f, FALSE, 0, Id, 3, TRUE);
  if (EFI_ERROR (Status)) { return Status; }
  if (Id[2] == 0x1a && ((Id[0] == 0xc8 && Id[1] == 0x67) ||
                         (Id[0] == 0xc2 && Id[1] == 0x20))) { return EFI_SUCCESS; }
  return EFI_UNSUPPORTED;
}

EFI_STATUS
Sg2042SpiReadUid (CONST SG2042_SPI_NOR *Nor, UINT8 Uid[16])
{
  UINT8 Flags, Data[17];
  EFI_STATUS Status = Command (Nor, 0x70, FALSE, 0, &Flags, 1, TRUE);
  if (EFI_ERROR (Status)) { return Status; }
  // RUID needs one more dummy byte when the chip is in four-byte address mode.
  UINTN Skip = Flags & BIT0;
  Status = Command (Nor, 0x4b, TRUE, 0, Data, 16 + Skip, TRUE);
  if (!EFI_ERROR (Status)) { CopyMem (Uid, Data + Skip, 16); }
  return Status;
}
