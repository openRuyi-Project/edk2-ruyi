/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/Fdt.h>
#include <Guid/RiscVSecHobData.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/K3BoardEepromLib.h>
#include <Library/K3HardwareLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiLib.h>
#define APBC_TWSI2       0xd4015038
#define EEPROM_ADDRESS   0x50
#define TARGET_COMPAT    "atmel,24c02"
#define TARGET_BASE      0xd4012000
#define PLL1_CONFIG      0xd4090100
#define PLL1_CONTROL     0xd4090104
#define PLL1_CONFIG2     0xd4090108
#define MPMU_STATUS      0xd4050010
#define MPMU_GATES       0xd4051024
#define CLOCK_MASK       (7U << 4 | BIT2 | BIT1 | BIT0)
#define ICR              0x00
#define ISR              0x04
#define IDBR             0x0c
#define IRCR             0x18
#define IBMR             0x1c
#define START            BIT0
#define STOP             BIT1
#define NAK              BIT2
#define TRANSFER         BIT3
#define UNIT_RESET       BIT10
#define MASTER_ABORT     BIT12
#define SCL_ENABLE       BIT13
#define UNIT_ENABLE      BIT14
#define GENERAL_DISABLE  BIT21
#define INTERRUPTS       (BIT18 | BIT19 | BIT20 | BIT22 | BIT23 | BIT24)
#define BUS_BUSY         (BIT15 | BIT16)
#define TX_EMPTY         BIT19
#define RX_FULL          BIT20
#define ARB_LOST         BIT18
#define BUS_ERROR        BIT22
#define ACK_MISSING      BIT14
#define EVENTS           (TX_EMPTY | RX_FULL | ARB_LOST | BUS_ERROR)
#define BYTE_TIMEOUT_US  2000

STATIC UINTN  mBase;

STATIC EFI_STATUS
WaitIdle (
  VOID
  )
{
  for (UINTN Us = 0; Us < BYTE_TIMEOUT_US; Us += 5) {
    if (!(MmioRead32 (mBase + ISR) & BUS_BUSY) &&
        !(MmioRead32 (mBase + ICR) & TRANSFER))
    {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (5);
  }

  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
Byte (
  UINT8    *Data,
  UINT32   Flags,
  BOOLEAN  Read
  )
{
  UINT32  Event = Read ? RX_FULL : TX_EMPTY;

  MmioWrite32 (mBase + ISR, EVENTS);
  if (!Read) {
    MmioWrite32 (mBase + IDBR, *Data);
  }

  MmioWrite32 (
    mBase + ICR,
    SCL_ENABLE | UNIT_ENABLE | GENERAL_DISABLE | TRANSFER | Flags
    );
  for (UINTN Us = 0; Us < BYTE_TIMEOUT_US; Us += 5) {
    UINT32  State = MmioRead32 (mBase + ISR);

    if (State & ARB_LOST) {
      return EFI_ABORTED;
    }

    if (State & BUS_ERROR) {
      return EFI_DEVICE_ERROR;
    }

    if ((State & Event) && !(MmioRead32 (mBase + ICR) & TRANSFER)) {
      if (!Read && (State & ACK_MISSING)) {
        return EFI_DEVICE_ERROR;
      }

      if (Read) {
        *Data = (UINT8)MmioRead32 (mBase + IDBR);
      }

      MmioWrite32 (mBase + ISR, Event);
      return EFI_SUCCESS;
    }

    MicroSecondDelay (5);
  }

  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
ReadRegisters (
  UINT8  Register,
  UINT8  *Data,
  UINTN  Count
  )
{
  EFI_STATUS  Status  = WaitIdle ();
  UINT8       Address = EEPROM_ADDRESS << 1;

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Byte (&Address, START, FALSE);
  if (!EFI_ERROR (Status)) {
    Status = Byte (&Register, 0, FALSE);
  }

  Address |= 1;
  if (!EFI_ERROR (Status)) {
    Status = Byte (&Address, START, FALSE);
  }

  for (UINTN Index = 0; !EFI_ERROR (Status) && (Index < Count); Index++) {
    Status = Byte (
               &Data[Index],
               (Index + 1 == Count) ? STOP | NAK : 0,
               TRUE
               );
  }

  if (!EFI_ERROR (Status)) {
    Status = WaitIdle ();
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "EEPROM read %02x failed: %r ICR=%08x ISR=%08x\r\n",
      Register,
      Status,
      MmioRead32 (mBase + ICR),
      MmioRead32 (mBase + ISR)
      ));
  }

  return Status;
}

STATIC EFI_STATUS
WritePage (
  UINT8        Register,
  CONST UINT8  *Data
  )
{
  EFI_STATUS  Status  = WaitIdle ();
  UINT8       Address = EEPROM_ADDRESS << 1;

  if (!EFI_ERROR (Status)) {
    Status = Byte (&Address, START, FALSE);
  }

  if (!EFI_ERROR (Status)) {
    Status = Byte (&Register, 0, FALSE);
  }

  for (UINTN I = 0; !EFI_ERROR (Status) && I < 8; I++) {
    UINT8  Value = Data[I];
    Status = Byte (&Value, I == 7 ? STOP : 0, FALSE);
  }

  if (!EFI_ERROR (Status)) {
    Status = WaitIdle ();
  }

  /* Match the vendor 24C02 driver write-cycle bound (10 ms). */
  MicroSecondDelay (10000);
  return Status;
}

EFI_STATUS
K3BoardEepromAccess (
  UINT8    *Eeprom,
  BOOLEAN  Write,
  UINTN    CommitOffset
  )
{
  VOID          *Fdt = NULL;
  EFI_STATUS    Status;
  INT32         Node, Bus, Length;
  CONST UINT32  *Reg;
  UINTN         Bytes;
  UINT32        SavedClock, SavedPll, SavedGate, SavedIcr, SavedIrcr;
  UINT32        SavedIsar, SavedIlcr, SavedIwcr;
  BOOLEAN       ControllerOwned = FALSE;
  UINT32        Pins[2], PadClock;
  BOOLEAN       PinsChanged = FALSE;

  if (!Eeprom || (Write && (CommitOffset > 252))) {
    return EFI_INVALID_PARAMETER;
  }

  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Fdt);
  if (EFI_ERROR (Status)) {
    CONST EFI_GUID     Guid = RISCV_SEC_HANDOFF_HOB_GUID;
    EFI_HOB_GUID_TYPE  *Hob = GetFirstGuidHob (&Guid);

    if (Hob && (GET_GUID_HOB_DATA_SIZE (Hob) >=
                sizeof (RISCV_SEC_HANDOFF_DATA)))
    {
      RISCV_SEC_HANDOFF_DATA  Handoff;

      CopyMem (&Handoff, GET_GUID_HOB_DATA (Hob), sizeof (Handoff));
      Fdt    = Handoff.FdtPointer;
      Status = Fdt ? EFI_SUCCESS : EFI_NOT_FOUND;
    }
  }

  if (EFI_ERROR (Status) || !Fdt || FdtCheckHeader (Fdt) ||
      (FdtTotalSize (Fdt) > SIZE_1MB) ||
      (FdtStringListSearch (
         Fdt,
         0,
         "compatible",
         "spacemit,k3-pico-itx"
         ) < 0))
  {
    return EFI_UNSUPPORTED;
  }

  Node = FdtNodeOffsetByCompatible (Fdt, -1, TARGET_COMPAT);
  Bus  = FdtParentOffset (Fdt, Node);
  Reg  = FdtGetProp (Fdt, Node, "reg", &Length);
  if (!K3FdtEnabled (Fdt, Node) || !Reg || (Length != 4) ||
      (Fdt32ToCpu (*Reg) != EEPROM_ADDRESS) ||
      (FdtStringListSearch (
         Fdt,
         Bus,
         "compatible",
         "spacemit,k3-i2c"
         ) < 0))
  {
    return EFI_UNSUPPORTED;
  }

  Status = K3FdtRegister (Fdt, Bus, 0, 0x20, &mBase, &Bytes);
  if (EFI_ERROR (Status) || (mBase != TARGET_BASE)) {
    return EFI_UNSUPPORTED;
  }

  STATIC CONST UINTN  Regions[] = {
    0xd4015000, 0xd4090000, 0xd4050000,
    TARGET_BASE & ~(UINTN)EFI_PAGE_MASK
  };

  for (UINTN Index = 0; Index < ARRAY_SIZE (Regions); Index++) {
    Status = K3MapMmio (Regions[Index], 0x2000);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  SavedClock = MmioRead32 (APBC_TWSI2);
  SavedPll   = MmioRead32 (PLL1_CONTROL);
  SavedGate  = MmioRead32 (MPMU_GATES);
  if ((MmioRead32 (PLL1_CONFIG) != 0x0b330ccc) ||
      (MmioRead32 (PLL1_CONFIG2) != 0xa0558989) ||
      !(MmioRead32 (MPMU_STATUS) & BIT24) ||
      ((SavedClock != BIT2) && (SavedClock != (BIT1 | BIT0))))
  {
    return EFI_NOT_READY;
  }

  /* Temporarily enable the TWSI2 clock path without changing the PLL rate.
     Controller ownership is checked before resetting or transferring. */
  MmioOr32 (PLL1_CONTROL, BIT3);
  MmioOr32 (MPMU_GATES, BIT6);
  MmioWrite32 (APBC_TWSI2, (SavedClock & ~CLOCK_MASK) | BIT1 | BIT0);
  MicroSecondDelay (10);
  Status = K3MapMmio (0xd401e000, 0x1000);
  if (EFI_ERROR (Status)) {
    goto RestoreClock;
  }

  /* EEPROM access uses pads 46/47 in mux mode 5. Restore their complete
     original words and pad clock when the transaction finishes. */
  PadClock = MmioRead32 (0xd401503c);
  MmioOr32 (0xd401503c, BIT0 | BIT1);
  Pins[0] = MmioRead32 (0xd401e0b8);
  Pins[1] = MmioRead32 (0xd401e0bc);
  MmioWrite32 (0xd401e0b8, 0xc045);
  MmioWrite32 (0xd401e0bc, 0xc045);
  PinsChanged = TRUE;
  SavedIcr    = MmioRead32 (mBase + ICR);
  SavedIrcr   = MmioRead32 (mBase + IRCR);
  SavedIsar   = MmioRead32 (mBase + 0x08);
  SavedIlcr   = MmioRead32 (mBase + 0x10);
  SavedIwcr   = MmioRead32 (mBase + 0x14);
  if ((SavedIcr & (INTERRUPTS | TRANSFER | START | STOP | MASTER_ABORT)) ||
      EFI_ERROR (WaitIdle ()) ||
      ((MmioRead32 (mBase + IBMR) & 3) != 3))
  {
    Status = EFI_ACCESS_DENIED;
    goto RestoreClock;
  }

  ControllerOwned = TRUE;
  MmioWrite32 (mBase + ICR, UNIT_RESET);
  MicroSecondDelay (10);
  MmioWrite32 (mBase + IRCR, SavedIrcr | BIT7);
  MmioWrite32 (
    mBase + ICR,
    SCL_ENABLE | UNIT_ENABLE | GENERAL_DISABLE
    );
  if (Write) {
    UINT8  Current[256];
    Status = ReadRegisters (0, Current, sizeof (Current));
    for (UINTN Phase = 0; !EFI_ERROR (Status) && Phase < 2; Phase++) {
      for (UINTN Offset = 0;
           !EFI_ERROR (Status) && Offset < 256; Offset += 8)
      {
        BOOLEAN  CommitPage = Offset / 8 >= CommitOffset / 8 &&
                              Offset / 8 <= (CommitOffset + 3) / 8;
        if ((CommitPage != (Phase == 1)) ||
            !CompareMem (Current + Offset, Eeprom + Offset, 8))
        {
          continue;
        }

        Status = WritePage ((UINT8)Offset, Eeprom + Offset);
        if (!EFI_ERROR (Status)) {
          Status = ReadRegisters ((UINT8)Offset, Current + Offset, 8);
          if (!EFI_ERROR (Status) &&
              CompareMem (Current + Offset, Eeprom + Offset, 8))
          {
            Status = EFI_DEVICE_ERROR;
          }
        }
      }
    }
  } else {
    Status = ReadRegisters (0, Eeprom, 256);
  }

RestoreClock:
  if (ControllerOwned) {
    /* On an error, terminate our transaction using only this controller.
       No clocks are removed while an owned byte transfer remains live. */
    MmioWrite32 (mBase + ICR, UNIT_RESET);
    MicroSecondDelay (10);
    MmioWrite32 (mBase + ICR, 0);
    if (EFI_ERROR (WaitIdle ())) {
      DEBUG ((DEBUG_ERROR, "EEPROM cleanup: bus not idle; temporary clocks retained.\r\n"));
      return EFI_DEVICE_ERROR;
    }

    MmioWrite32 (mBase + ISR, EVENTS);
    MmioWrite32 (mBase + 0x08, SavedIsar);
    MmioWrite32 (mBase + 0x10, SavedIlcr);
    MmioWrite32 (mBase + 0x14, SavedIwcr);
    MmioWrite32 (mBase + IRCR, SavedIrcr);
    MmioWrite32 (mBase + ICR, SavedIcr);
  }

  if (PinsChanged) {
    MmioWrite32 (0xd401e0b8, Pins[0]);
    MmioWrite32 (0xd401e0bc, Pins[1]);
    MmioWrite32 (0xd401503c, PadClock);
  }

  MmioWrite32 (APBC_TWSI2, SavedClock);
  MmioAndThenOr32 (MPMU_GATES, ~BIT6, SavedGate & BIT6);
  MmioAndThenOr32 (PLL1_CONTROL, ~BIT3, SavedPll & BIT3);
  if ((MmioRead32 (APBC_TWSI2) != SavedClock) ||
      ((MmioRead32 (PLL1_CONTROL) & BIT3) != (SavedPll & BIT3)) ||
      ((MmioRead32 (MPMU_GATES) & BIT6) != (SavedGate & BIT6)))
  {
    return EFI_DEVICE_ERROR;
  }

  return Status;
}
