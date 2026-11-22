/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/DwI2cReadLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#define IC_CON            0x00
#define IC_TAR            0x04
#define IC_DATA_CMD       0x10
#define IC_INTR_MASK      0x30
#define IC_RAW_INTR_STAT  0x34
#define IC_CLR_INTR       0x40
#define IC_ENABLE         0x6c
#define IC_STATUS         0x70
#define IC_RXFLR          0x78
#define IC_ENABLE_STATUS  0x9c

#define IC_ENABLE_ABORT         BIT1
#define IC_ENABLE_TX_CMD_BLOCK  BIT2
#define IC_STATUS_ACTIVITY      BIT0
#define IC_STATUS_TFE           BIT2

STATIC EFI_STATUS
Wait (UINTN Base, UINTN Reg, UINT32 Mask, UINT32 Value)
{
  for (UINTN Us = 0; Us < 10000; Us++) {
    if (Reg != IC_ENABLE_STATUS && (MmioRead32 (Base + IC_RAW_INTR_STAT) & BIT6)) {
      return EFI_DEVICE_ERROR; // TX_ABRT, including address NACK
    }
    if ((MmioRead32 (Base + Reg) & Mask) == Value) { return EFI_SUCCESS; }
    MicroSecondDelay (1);
  }
  return EFI_TIMEOUT;
}

EFI_STATUS
EFIAPI
DwI2cReadRegisters (UINTN Base, UINT8 Address, UINT8 Register, UINT8 *Data, UINTN Count)
{
  if (Base == 0 || Address > 0x7f || Data == NULL || Count == 0) { return EFI_INVALID_PARAMETER; }
  UINT32 Enabled = MmioRead32 (Base + IC_ENABLE);
  UINT32 BusStatus = MmioRead32 (Base + IC_STATUS);
  // Disabling the controller discards queued commands, even on an idle bus.
  if ((Enabled & (IC_ENABLE_ABORT | IC_ENABLE_TX_CMD_BLOCK)) ||
      (BusStatus & (IC_STATUS_ACTIVITY | IC_STATUS_TFE)) != IC_STATUS_TFE ||
      MmioRead32 (Base + IC_RXFLR) != 0) {
    return EFI_NOT_READY;
  }

  Enabled &= BIT0;
  UINT32 Control = MmioRead32 (Base + IC_CON);
  UINT32 Target = MmioRead32 (Base + IC_TAR);
  UINT32 Mask = MmioRead32 (Base + IC_INTR_MASK);
  MmioWrite32 (Base + IC_INTR_MASK, 0);
  MmioWrite32 (Base + IC_ENABLE, 0);
  EFI_STATUS Status = Wait (Base, IC_ENABLE_STATUS, BIT0, 0);
  if (!EFI_ERROR (Status)) {
    MmioRead32 (Base + IC_CLR_INTR);
    // Standard-speed master, repeated START, slave disabled.
    MmioWrite32 (Base + IC_CON, BIT0 | BIT1 | BIT5 | BIT6);
    MmioWrite32 (Base + IC_TAR, Address);
    MmioWrite32 (Base + IC_ENABLE, 1);
    Status = Wait (Base, IC_ENABLE_STATUS, BIT0, BIT0);
  }
  if (!EFI_ERROR (Status)) {
    // Set the register pointer, then read the requested bytes in one transfer.
    MmioWrite32 (Base + IC_DATA_CMD, Register);
    for (UINTN I = 0; I < Count; I++) {
      Status = Wait (Base, IC_STATUS, BIT1, BIT1);
      if (EFI_ERROR (Status)) { break; }
      UINT32 Cmd = BIT8;
      if (I == 0) { Cmd |= BIT10; }
      if (I == Count - 1) { Cmd |= BIT9; }
      MmioWrite32 (Base + IC_DATA_CMD, Cmd);
      Status = Wait (Base, IC_STATUS, BIT3, BIT3);
      if (EFI_ERROR (Status)) { break; }
      Data[I] = (UINT8)MmioRead32 (Base + IC_DATA_CMD);
    }
    if (!EFI_ERROR (Status)) { Status = Wait (Base, IC_RAW_INTR_STAT, BIT9, BIT9); }
  }

  MmioWrite32 (Base + IC_ENABLE, 0);
  EFI_STATUS Stopped = Wait (Base, IC_ENABLE_STATUS, BIT0, 0);
  if (!EFI_ERROR (Stopped)) {
    MmioRead32 (Base + IC_CLR_INTR);
    MmioWrite32 (Base + IC_CON, Control);
    MmioWrite32 (Base + IC_TAR, Target);
  }
  MmioWrite32 (Base + IC_ENABLE, Enabled);
  EFI_STATUS Restored = Wait (Base, IC_ENABLE_STATUS, BIT0, Enabled);
  MmioWrite32 (Base + IC_INTR_MASK, Mask);
  if (EFI_ERROR (Status)) { return Status; }
  return EFI_ERROR (Stopped) ? Stopped : Restored;
}
