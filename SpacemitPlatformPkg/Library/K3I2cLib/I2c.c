/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/K3I2cLib.h>
#include <Library/TimerLib.h>

#define ICR              0x00
#define ISR              0x04
#define IDBR             0x0c
#define START            BIT0
#define STOP             BIT1
#define NAK              BIT2
#define TRANSFER         BIT3
#define SCL_ENABLE       BIT13
#define UNIT_ENABLE      BIT14
#define GENERAL_DISABLE  BIT21
#define BUS_BUSY         (BIT15 | BIT16)
#define TX_EMPTY         BIT19
#define RX_FULL          BIT20
#define ARB_LOST         BIT18
#define BUS_ERROR        BIT22
#define ACK_MISSING      BIT14
#define EVENTS           (TX_EMPTY | RX_FULL | ARB_LOST | BUS_ERROR)
#define BYTE_TIMEOUT_US  2000

EFI_STATUS
K3I2cWaitIdle (
  UINTN  Base
  )
{
  for (UINTN Us = 0; Us < BYTE_TIMEOUT_US; Us += 5) {
    if (!(MmioRead32 (Base + ISR) & BUS_BUSY) &&
        !(MmioRead32 (Base + ICR) & TRANSFER))
    {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (5);
  }

  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
Byte (
  UINTN    Base,
  UINT8    *Data,
  UINT32   Flags,
  BOOLEAN  Read
  )
{
  UINT32  Event = Read ? RX_FULL : TX_EMPTY;

  MmioWrite32 (Base + ISR, EVENTS);
  if (!Read) {
    MmioWrite32 (Base + IDBR, *Data);
  }

  MmioWrite32 (
    Base + ICR,
    SCL_ENABLE | UNIT_ENABLE | GENERAL_DISABLE | TRANSFER | Flags
    );
  for (UINTN Us = 0; Us < BYTE_TIMEOUT_US; Us += 5) {
    UINT32  State = MmioRead32 (Base + ISR);

    if (State & ARB_LOST) {
      return EFI_ABORTED;
    }

    if (State & BUS_ERROR) {
      return EFI_DEVICE_ERROR;
    }

    if ((State & Event) && !(MmioRead32 (Base + ICR) & TRANSFER)) {
      if (!Read && (State & ACK_MISSING)) {
        return EFI_DEVICE_ERROR;
      }

      if (Read) {
        *Data = (UINT8)MmioRead32 (Base + IDBR);
      }

      MmioWrite32 (Base + ISR, Event);
      return EFI_SUCCESS;
    }

    MicroSecondDelay (5);
  }

  return EFI_TIMEOUT;
}

EFI_STATUS
K3I2cReadRegisters (
  UINTN  Base,
  UINT8  Address,
  UINT8  Register,
  UINT8  *Data,
  UINTN  Count
  )
{
  if (!Base || (Base & 3) || (Address > 0x7f) || !Data || !Count) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_STATUS  Status = K3I2cWaitIdle (Base);
  UINT8       Target = Address << 1;

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Byte (Base, &Target, START, FALSE);
  if (!EFI_ERROR (Status)) {
    Status = Byte (Base, &Register, 0, FALSE);
  }

  Target |= 1;
  if (!EFI_ERROR (Status)) {
    Status = Byte (Base, &Target, START, FALSE);
  }

  for (UINTN Index = 0; !EFI_ERROR (Status) && (Index < Count); Index++) {
    Status = Byte (Base, &Data[Index],
                   (Index + 1 == Count) ? STOP | NAK : 0, TRUE);
  }

  return EFI_ERROR (Status) ? Status : K3I2cWaitIdle (Base);
}

EFI_STATUS
K3I2cWriteRegisters (
  UINTN        Base,
  UINT8        Address,
  UINT8        Register,
  CONST UINT8  *Data,
  UINTN        Count
  )
{
  if (!Base || (Base & 3) || (Address > 0x7f) || !Data || !Count) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_STATUS  Status = K3I2cWaitIdle (Base);
  UINT8       Target = Address << 1;

  if (!EFI_ERROR (Status)) {
    Status = Byte (Base, &Target, START, FALSE);
  }

  if (!EFI_ERROR (Status)) {
    Status = Byte (Base, &Register, 0, FALSE);
  }

  for (UINTN Index = 0; !EFI_ERROR (Status) && (Index < Count); Index++) {
    UINT8  Value = Data[Index];
    Status = Byte (Base, &Value, Index + 1 == Count ? STOP : 0, FALSE);
  }

  return EFI_ERROR (Status) ? Status : K3I2cWaitIdle (Base);
}
