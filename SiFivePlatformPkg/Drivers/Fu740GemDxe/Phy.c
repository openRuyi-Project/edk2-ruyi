/* SPDX-License-Identifier: MIT */

/* VSC8541 initialization adapted from U-Boot drivers/net/phy/mscc.c.
 * Copyright (c) 2016 Microsemi Corporation
 * Author: John Haechten
 */
#include <Library/BaseLib.h>
#include <Library/TimerLib.h>
#include "Gem.h"

#define PHY_PAGE          31
#define PHY_STANDARD      0
#define PHY_EXTENDED2     2
#define PHY_TOKEN_RING    0x52b5
#define PHY_TR_ADDRESS    16
#define PHY_TR_LOW        17
#define PHY_TR_HIGH       18
#define PHY_TR_READ       0xa000
#define PHY_TR_WRITE      0x8000
#define PHY_TR_LINK       0x07f8
#define PHY_TR_THRESHOLD  0x0fa4
#define PHY_TR_GAIN       0x0f92

STATIC EFI_STATUS
Write (
  GEM_PORT  *Port,
  UINT32    Register,
  UINT16    Value
  )
{
  return GemMdio (Port, Register, FALSE, &Value);
}

STATIC EFI_STATUS
Modify (
  GEM_PORT  *Port,
  UINT32    Register,
  UINT16    Mask,
  UINT16    Value
  )
{
  UINT16      Current;
  EFI_STATUS  Status = GemMdio (Port, Register, TRUE, &Current);

  return EFI_ERROR (Status) ? Status : Write (Port, Register, (Current & ~Mask) | Value);
}

EFI_STATUS
GemPhyConfigure (
  GEM_PORT  *Port
  )
{
  EFI_STATUS  Status, Restore;
  UINT16      Control;

  if ((Port->PhyId & 0xfffffff0) != 0x00070770) {
    return EFI_UNSUPPORTED;
  }

  // Microsemi's silicon-validation updates for link detect and analog gain.
  #define STEP(Operation)  do { Status = (Operation); if (EFI_ERROR (Status)) { goto Done; } } while (0)
  STEP (Write (Port, PHY_PAGE, PHY_TOKEN_RING));
  STEP (Write (Port, PHY_TR_ADDRESS, PHY_TR_READ | PHY_TR_LINK));
  STEP (Modify (Port, PHY_TR_LOW, 0x0018, 0x0018));
  STEP (Write (Port, PHY_TR_ADDRESS, PHY_TR_WRITE | PHY_TR_LINK));
  STEP (Write (Port, PHY_TR_ADDRESS, PHY_TR_READ | PHY_TR_THRESHOLD));
  STEP (Modify (Port, PHY_TR_HIGH, 0x007f, 0x0018));
  STEP (Write (Port, PHY_TR_ADDRESS, PHY_TR_WRITE | PHY_TR_THRESHOLD));
  STEP (Write (Port, PHY_TR_ADDRESS, PHY_TR_READ | PHY_TR_GAIN));
  STEP (Modify (Port, PHY_TR_HIGH, 0x0001, 0));
  STEP (Modify (Port, PHY_TR_LOW, 0xf000, 0x1000));
  STEP (Write (Port, PHY_TR_ADDRESS, PHY_TR_WRITE | PHY_TR_GAIN));
  STEP (Write (Port, PHY_PAGE, PHY_STANDARD));
  STEP (Modify (Port, 23, BIT11 | BIT12, 0)); // GMII MAC interface
  STEP (Write (Port, PHY_PAGE, PHY_EXTENDED2));
  STEP (Modify (Port, 20, BIT11, BIT11)); // No recovered clock on RX_CLK output
  STEP (Write (Port, PHY_PAGE, PHY_STANDARD));
  STEP (Modify (Port, 0, 0, BIT15));
  for (UINTN Retry = 0; Retry < 500; Retry++) {
    STEP (GemMdio (Port, 0, TRUE, &Control));
    if (!(Control & BIT15)) {
      break;
    }

    if (Retry == 499) {
      Status = EFI_TIMEOUT;
      goto Done;
    }

    MicroSecondDelay (1000);
  }

  // Advertise 10/100/1000, without pause (the firmware does not enable flow control).
  STEP (Modify (Port, 4, 0x0fe0, 0x01e0));
  STEP (Modify (Port, 9, BIT8 | BIT9, BIT8 | BIT9));
  STEP (Write (Port, 0, BIT12 | BIT9));
Done:
  Restore = Write (Port, PHY_PAGE, PHY_STANDARD);
  return EFI_ERROR (Status) ? Status : Restore;
  #undef STEP
}
