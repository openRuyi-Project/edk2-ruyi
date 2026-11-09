/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtBoardInfoLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/PlatformMacAddressLib.h>
#include <Library/TimerLib.h>

#define IC_CON             0x00
#define IC_TAR             0x04
#define IC_DATA_CMD        0x10
#define IC_SS_HCNT         0x14
#define IC_SS_LCNT         0x18
#define IC_INTR_MASK       0x30
#define IC_RAW_INTR        0x34
#define IC_RX_TL           0x38
#define IC_TX_TL           0x3c
#define IC_CLEAR_INTR      0x40
#define IC_ENABLE          0x6c
#define IC_STATUS          0x70
#define IC_ENABLE_STATUS   0x9c
#define IC_COMPONENT_TYPE  0xfc
#define IC_ABORT           BIT6
#define IC_STOP            BIT9
#define IC_RESTART         BIT10
#define IC_READ            BIT8
#define SYS_I2C0_CLOCK     0x210
#define SYS_CONFIG_CLOCK   0x20c
#define SYS_I2C0_RESET     0x198
#define EEPROM_ADDRESS     0x50
#define BOARD_INFO_BYTES   51
#define BOARD_INFO_CRC     47
#define BOARD_INFO_MAC     29
#define BOARD_INFO_BACKUP  80
#define BOARD_INFO_MAGIC  0x45505ef1

STATIC UINT8 mMac[2][6];
STATIC BOOLEAN mValid;

STATIC EFI_STATUS
Cell (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name, UINT32 *Value)
{
  INT32 Length;
  CONST UINT32 *P = FdtGetProp (Fdt, Node, Name, &Length);
  if (!P || Length != 4) { return EFI_COMPROMISED_DATA; }
  *Value = Fdt32ToCpu (*P);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Wait (UINTN Base, UINTN Register, UINT32 Mask, UINT32 Value)
{
  UINTN Us;
  for (Us = 0; Us < 20000; Us++) {
    if ((MmioRead32 (Base + Register) & Mask) == Value) { return EFI_SUCCESS; }
    MicroSecondDelay (1);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
Enable (UINTN Base, BOOLEAN On)
{
  MmioWrite32 (Base + IC_ENABLE, On ? 1 : 0);
  return Wait (Base, IC_ENABLE_STATUS, BIT0, On ? BIT0 : 0);
}

STATIC EFI_STATUS
ReceiveByte (UINTN Base, UINT8 *Byte)
{
  UINTN Us;
  for (Us = 0; Us < 20000; Us++) {
    if (MmioRead32 (Base + IC_RAW_INTR) & IC_ABORT) { return EFI_DEVICE_ERROR; }
    if (MmioRead32 (Base + IC_STATUS) & BIT3) {
      *Byte = MmioRead32 (Base + IC_DATA_CMD);
      return EFI_SUCCESS;
    }
    MicroSecondDelay (1);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
ReadRecord (UINTN Base, UINT8 Offset, UINT8 *Data)
{
  EFI_STATUS Status, Cleanup;
  UINTN Index;
  Status = Enable (Base, FALSE);
  if (EFI_ERROR (Status)) { return Status; }
  MmioWrite32 (Base + IC_TAR, EEPROM_ADDRESS);
  MmioRead32 (Base + IC_CLEAR_INTR);
  Status = Enable (Base, TRUE);
  if (EFI_ERROR (Status)) { return Status; }
  // This write supplies the EEPROM byte address, never EEPROM contents.
  MmioWrite32 (Base + IC_DATA_CMD, Offset);
  for (Index = 0; Index < BOARD_INFO_BYTES; Index++) {
    MmioWrite32 (Base + IC_DATA_CMD, IC_READ | (Index == 0 ? IC_RESTART : 0) |
                 (Index == BOARD_INFO_BYTES - 1 ? IC_STOP : 0));
    Status = ReceiveByte (Base, Data + Index);
    if (EFI_ERROR (Status)) { break; }
  }
  if (!EFI_ERROR (Status)) {
    Status = Wait (Base, IC_RAW_INTR, IC_STOP, IC_STOP);
  }
  if (EFI_ERROR (Status)) {
    // Abort an incomplete combined transfer before disabling the controller.
    MmioWrite32 (Base + IC_ENABLE, BIT0 | BIT1);
    Cleanup = Wait (Base, IC_ENABLE, BIT1, 0);
    if (EFI_ERROR (Cleanup)) { return Cleanup; }
  }
  Cleanup = Enable (Base, FALSE);
  MmioRead32 (Base + IC_CLEAR_INTR);
  return EFI_ERROR (Status) ? Status : Cleanup;
}

STATIC BOOLEAN
ValidRecord (CONST UINT8 *Data)
{
  UINT32 Crc = 0;
  UINTN I, Bit;
  UINT8 Any;
  // SiFive carrier records use CRC32 with the U-Boot seed 0xffffffff.
  if (ReadUnaligned32 ((CONST UINT32 *)Data) != BOARD_INFO_MAGIC || Data[4] != 3) { return FALSE; }
  for (I = 0; I < BOARD_INFO_CRC; I++) {
    Crc ^= Data[I];
    for (Bit = 0; Bit < 8; Bit++) {
      Crc = (Crc >> 1) ^ ((Crc & 1) ? 0xedb88320U : 0);
    }
  }
  if (~Crc != ReadUnaligned32 ((CONST UINT32 *)(Data + BOARD_INFO_CRC))) { return FALSE; }
  for (I = 0; I < 2; I++) {
    CONST UINT8 *Mac = Data + BOARD_INFO_MAC + 6 * I;
    Any = Mac[0] | Mac[1] | Mac[2] | Mac[3] | Mac[4] | Mac[5];
    if (!Any || (Mac[0] & 1)) { return FALSE; }
  }
  return CompareMem (Data + BOARD_INFO_MAC, Data + BOARD_INFO_MAC + 6, 6) != 0;
}

STATIC EFI_STATUS
LoadAddresses (VOID)
{
  RUYI_BOARD_INFO Info;
  CONST VOID *Fdt;
  CONST UINT32 *Data;
  INT32 Node = -1, Bus, Provider, Pinctrl, Length;
  UINT32 Value, Rate, SavedClock;
  UINTN Base, Clock, Reset, Pins;
  UINT8 Record[BOARD_INFO_BYTES];
  EFI_STATUS Status;

  Status = FdtBoardInfoGet (&Info, &Fdt);
  if (EFI_ERROR (Status)) { return Status; }
  while ((Node = FdtNodeOffsetByCompatible (Fdt, Node, "atmel,24c02")) >= 0) {
    Bus = FdtParentOffset (Fdt, Node);
    if (!FdtNodeEnabled (Fdt, Node) || EFI_ERROR (Cell (Fdt, Node, "reg", &Value)) || Value != EEPROM_ADDRESS) { continue; }
    if (FdtStringListSearch (Fdt, Bus, "compatible", "eswin,eic7700-i2c") >= 0) { break; }
  }
  if (Node < 0) { return EFI_NOT_FOUND; }
  Status = FdtGetMmio (Fdt, Bus, 0, 0x100, &Base, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Data = FdtGetProp (Fdt, Bus, "clocks", &Length);
  if (!Data || Length != 8 || Fdt32ToCpu (Data[1]) != 235) { return EFI_UNSUPPORTED; }
  Provider = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Data));
  if (FdtStringListSearch (Fdt, Provider, "compatible", "eswin,eic7700-clock") < 0) { return EFI_UNSUPPORTED; }
  Status = FdtGetMmio (Fdt, Provider, 0, SYS_I2C0_CLOCK + 4, &Clock, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Data = FdtGetProp (Fdt, Bus, "resets", &Length);
  if (!Data || Length != 8 || Fdt32ToCpu (Data[1]) != 201) { return EFI_UNSUPPORTED; }
  Provider = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Data));
  if (FdtStringListSearch (Fdt, Provider, "compatible", "eswin,eic7700-reset") < 0) { return EFI_UNSUPPORTED; }
  Status = FdtGetMmio (Fdt, Provider, 0, SYS_I2C0_RESET + 4, &Reset, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Cell (Fdt, Bus, "pinctrl-0", &Value);
  if (EFI_ERROR (Status)) { return Status; }
  Provider = FdtNodeOffsetByPhandle (Fdt, Value);
  if (FdtStringListCount (Fdt, Provider, "pins") != 2 ||
      FdtStringListSearch (Fdt, Provider, "pins", "i2c10_scl") < 0 ||
      FdtStringListSearch (Fdt, Provider, "pins", "i2c10_sda") < 0 ||
      FdtStringListSearch (Fdt, Provider, "function", "i2c") < 0) { return EFI_UNSUPPORTED; }
  Pinctrl = Provider;
  do { Pinctrl = FdtParentOffset (Fdt, Pinctrl); }
  while (Pinctrl > 0 && FdtStringListSearch (Fdt, Pinctrl, "compatible", "eswin,eic7700-pinctrl") < 0);
  if (Pinctrl <= 0) { return EFI_NOT_FOUND; }
  Status = FdtGetMmio (Fdt, Pinctrl, 0, 151 * 4, &Pins, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (Base, 0x100);
  if (!EFI_ERROR (Status)) { Status = FdtMapMmio (Clock, SYS_I2C0_CLOCK + 4); }
  if (!EFI_ERROR (Status)) { Status = FdtMapMmio (Reset, SYS_I2C0_RESET + 4); }
  if (!EFI_ERROR (Status)) { Status = FdtMapMmio (Pins, 151 * 4); }
  if (EFI_ERROR (Status)) { return Status; }
  Value = MmioRead32 (Clock + SYS_CONFIG_CLOCK);
  Rate = (Value & BIT0) ? 24000000 : 400000000U / MAX (2U, (Value >> 4) & 7);
  SavedClock = MmioRead32 (Clock + SYS_I2C0_CLOCK);
  MmioOr32 (Clock + SYS_I2C0_CLOCK, BIT31);
  MmioOr32 (Reset + SYS_I2C0_RESET, BIT0);
  MmioAndThenOr32 (Pins + 149 * 4, ~(7U << 16), BIT0);
  MmioAndThenOr32 (Pins + 150 * 4, ~(7U << 16), BIT0);
  if (MmioRead32 (Base + IC_COMPONENT_TYPE) != 0x44570140) { Status = EFI_UNSUPPORTED; goto Done; }
  Status = Wait (Base, IC_STATUS, BIT5, 0);
  if (EFI_ERROR (Status)) { goto Done; }
  Status = Enable (Base, FALSE);
  if (EFI_ERROR (Status)) { goto Done; }
  MmioWrite32 (Base + IC_CON, BIT0 | BIT1 | BIT5 | BIT6);
  // Standard mode: tHIGH 4.0 us, tLOW 4.7 us, rise/fall allowance 0.3 us.
  MmioWrite32 (Base + IC_SS_HCNT, DivU64x32 (MultU64x32 (Rate, 4300), 1000000000) - 3);
  MmioWrite32 (Base + IC_SS_LCNT, DivU64x32 (MultU64x32 (Rate, 5000), 1000000000) - 1);
  MmioWrite32 (Base + IC_INTR_MASK, 0);
  MmioWrite32 (Base + IC_RX_TL, 0);
  MmioWrite32 (Base + IC_TX_TL, 0);
  Status = ReadRecord (Base, 0, Record);
  if (EFI_ERROR (Status)) { goto Done; }
  if (!ValidRecord (Record)) {
    Status = ReadRecord (Base, BOARD_INFO_BACKUP, Record);
    if (EFI_ERROR (Status)) { goto Done; }
  }
  if (!ValidRecord (Record)) { Status = EFI_COMPROMISED_DATA; goto Done; }
  CopyMem (mMac, Record + BOARD_INFO_MAC, sizeof (mMac));
  mValid = TRUE;
Done:
  // No DMA is used. A failed transaction is held in reset before gating.
  if (EFI_ERROR (Status)) { MmioAnd32 (Reset + SYS_I2C0_RESET, ~((UINT32)BIT0)); }
  if (!(SavedClock & BIT31)) { MmioAnd32 (Clock + SYS_I2C0_CLOCK, ~((UINT32)BIT31)); }
  return Status;
}

EFI_STATUS
PlatformGetMacAddress (UINTN Index, UINT8 Mac[6])
{
  EFI_STATUS Status;
  if (!Mac || Index >= ARRAY_SIZE (mMac)) { return EFI_INVALID_PARAMETER; }
  if (!mValid) {
    Status = LoadAddresses ();
    if (EFI_ERROR (Status)) { return Status; }
  }
  CopyMem (Mac, mMac[Index], 6);
  return EFI_SUCCESS;
}
