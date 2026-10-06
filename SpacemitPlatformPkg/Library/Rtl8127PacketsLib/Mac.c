/* SPDX-License-Identifier: GPL-2.0-only */

/* RTL8127A MAC sequence adapted from Linux r8169_main.c,
   ee6f8ef68a77339e4c210784e941119fdae55dd5.
   Copyright (c) 2002 ShuChen <shuchen@realtek.com.tw>
   Copyright (c) 2003-2007 Francois Romieu <romieu@fr.zoreil.com>
   Additional contributions by the Linux r8169 authors.
   Only version 80 is implemented. Polling keeps interrupts and ASPM off. */
#include "Internal.h"
#include <Library/Rtl8127Lib.h>
#include <Library/TimerLib.h>

EFI_STATUS
R27Read (
  EFI_PCI_IO_PROTOCOL        *Pci,
  EFI_PCI_IO_PROTOCOL_WIDTH  Width,
  UINT32                     Register,
  VOID                       *Value
  )
{
  return Pci->Mem.Read (Pci, Width, 2, Register, 1, Value);
}

EFI_STATUS
R27Write (
  EFI_PCI_IO_PROTOCOL        *Pci,
  EFI_PCI_IO_PROTOCOL_WIDTH  Width,
  UINT32                     Register,
  UINT32                     Value
  )
{
  return Pci->Mem.Write (Pci, Width, 2, Register, 1, &Value);
}

STATIC EFI_STATUS
Modify (
  EFI_PCI_IO_PROTOCOL        *Pci,
  EFI_PCI_IO_PROTOCOL_WIDTH  Width,
  UINT32                     Register,
  UINT32                     Clear,
  UINT32                     Set
  )
{
  UINT32      Value  = 0;
  EFI_STATUS  Status = R27Read (Pci, Width, Register, &Value);

  return EFI_ERROR (Status) ? Status :
         R27Write (Pci, Width, Register, (Value & ~Clear) | Set);
}

STATIC EFI_STATUS
Ocp (
  EFI_PCI_IO_PROTOCOL  *Pci,
  UINT16               Register,
  UINT16               Clear,
  UINT16               Set
  )
{
  UINT32      Value;
  EFI_STATUS  Status = R27Write (
                         Pci,
                         EfiPciIoWidthUint32,
                         0xb0,
                         (UINT32)Register << 15
                         );

  if (!EFI_ERROR (Status)) {
    Status = R27Read (Pci, EfiPciIoWidthUint32, 0xb0, &Value);
  }

  return EFI_ERROR (Status) ? Status : R27Write (
                                         Pci,
                                         EfiPciIoWidthUint32,
                                         0xb0,
                                         BIT31 | ((UINT32)Register << 15) |
                                         (UINT16)(((UINT16)Value & ~Clear) | Set)
                                         );
}

EFI_STATUS
R27Reset (
  EFI_PCI_IO_PROTOCOL  *Pci
  )
{
  EFI_STATUS  Status = R27Write (Pci, EfiPciIoWidthUint8, 0x37, BIT4);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (UINTN Us = 0; Us < 10000; Us += 100) {
    UINT8  Command;

    Status = R27Read (Pci, EfiPciIoWidthUint8, 0x37, &Command);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (!(Command & (BIT4 | BIT3 | BIT2))) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (100);
  }

  return EFI_TIMEOUT;
}

EFI_STATUS
R27PrepareMac (
  EFI_PCI_IO_PROTOCOL  *Pci,
  CONST UINT8          Mac[6]
  )
{
  STATIC CONST struct {
    UINT16    Register, Clear, Set;
  } Settings[] = {
    { 0xe092, 0x00ff, 0      },
    { 0xd40a, 0x0010, 0      },
    { 0xc140, 0xffff, 0xffff },
    { 0xc142, 0xffff, 0xffff },
    { 0xd3e2, 0x0fff, 0x03a9 },
    { 0xd3e4, 0x00ff, 0      },
    { 0xe860, 0,      0x0080 },
    { 0xeb58, 1,      0      },
    { 0xe614, 0x0f00, 0x0f00 },
    { 0xe63e, 0x0c30, 0x0020 },
    { 0xc0b4, 0,      0x000c },
    { 0xeb6a, 0x00ff, 0x0033 },
    { 0xeb50, 0x03e0, 0x0040 },
    { 0xe056, 0x00f0, 0      },
    { 0xe040, 0x1000, 0      },
    { 0xea1c, 0x0303, 1      },
    { 0xe0c0, 0x4f0f, 0x4403 },
    { 0xe052, 0x0080, 0x0068 },
    { 0xd430, 0x0fff, 0x047f },
    { 0xea84, 0,      3      }
  };
  EFI_STATUS  Status = R27Reset (Pci);

  if (!EFI_ERROR (Status)) {
    Status = Rtl8127InitializeSfp (Pci);
  }

  if (!EFI_ERROR (Status)) {
    Status = R27Write (Pci, EfiPciIoWidthUint8, 0x50, 0xc0);
  }

  if (!EFI_ERROR (Status)) {
    Status = Modify (Pci, EfiPciIoWidthUint8, 0x54, BIT1, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = Modify (Pci, EfiPciIoWidthUint8, 0x56, BIT0, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = R27Write (Pci, EfiPciIoWidthUint8, 0x34, 0);
  }

  for (UINTN I = 0; !EFI_ERROR (Status) && I < ARRAY_SIZE (Settings); I++) {
    Status = Ocp (
               Pci,
               Settings[I].Register,
               Settings[I].Clear,
               Settings[I].Set
               );
  }

  if (!EFI_ERROR (Status)) {
    Status = R27Write (Pci, EfiPciIoWidthUint16, 0x382, 0x221b);
  }

  if (!EFI_ERROR (Status)) {
    Status = R27Write (Pci, EfiPciIoWidthUint32, 0x4500, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = R27Write (Pci, EfiPciIoWidthUint16, 0x4800, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = Modify (Pci, EfiPciIoWidthUint8, 0x52, BIT4, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = Modify (Pci, EfiPciIoWidthUint8, 0xd8, BIT1, 0);
  }

  for (UINTN Offset = 0xa00; !EFI_ERROR (Status) && Offset < 0xb00;
       Offset += 4)
  {
    Status = R27Write (Pci, EfiPciIoWidthUint32, Offset, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = R27Write (Pci, EfiPciIoWidthUint16, 0x7a, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = Ocp (Pci, 0xea1c, 4, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = Ocp (Pci, 0xeb54, 0, 1);
  }

  if (!EFI_ERROR (Status)) {
    MicroSecondDelay (1);
    Status = Ocp (Pci, 0xeb54, 1, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = Modify (Pci, EfiPciIoWidthUint16, 0x1880, 0x30, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = Ocp (Pci, 0xe098, 0xffff, 0xc302);
  }

  if (!EFI_ERROR (Status)) {
    Status = EFI_TIMEOUT;
    for (UINTN Attempt = 0; Attempt < 10; Attempt++) {
      UINT32  Value;

      EFI_STATUS  ReadStatus = R27Write (
                                 Pci,
                                 EfiPciIoWidthUint32,
                                 0xb0,
                                 0xe00eU << 15
                                 );
      if (!EFI_ERROR (ReadStatus)) {
        ReadStatus = R27Read (Pci, EfiPciIoWidthUint32, 0xb0, &Value);
      }

      if (EFI_ERROR (ReadStatus)) {
        Status = ReadStatus;
        break;
      }

      if (!(Value & BIT13)) {
        Status = EFI_SUCCESS;
        break;
      }

      MicroSecondDelay (1000);
    }
  }

  if (!EFI_ERROR (Status)) {
    Status = Modify (Pci, EfiPciIoWidthUint32, 0xf0, BIT19, 0);
  }

  if (!EFI_ERROR (Status)) {
    /* 8127 uses 64-bit descriptor addresses directly. PCIDAC in the
       older CPlusCmd layout is not needed by the version-80 driver. */
    Status = Modify (Pci, EfiPciIoWidthUint16, 0xe0, ~0x2003U, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = R27Write (
               Pci,
               EfiPciIoWidthUint16,
               4,
               (UINT16)Mac[4] | (UINT16)Mac[5] << 8
               );
  }

  if (!EFI_ERROR (Status)) {
    Status = R27Write (
               Pci,
               EfiPciIoWidthUint32,
               0,
               (UINT32)Mac[0] | (UINT32)Mac[1] << 8 |
               (UINT32)Mac[2] << 16 | (UINT32)Mac[3] << 24
               );
  }

  EFI_STATUS  Lock = R27Write (Pci, EfiPciIoWidthUint8, 0x50, 0);

  return EFI_ERROR (Status) ? Status : Lock;
}

EFI_STATUS
R27Drain (
  EFI_PCI_IO_PROTOCOL  *Pci
  )
{
  EFI_STATUS  Status = R27Write (Pci, EfiPciIoWidthUint32, 0x38, 0);

  if (!EFI_ERROR (Status)) {
    Status = Modify (Pci, EfiPciIoWidthUint32, 0x44, 0x3f, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = Modify (Pci, EfiPciIoWidthUint32, 0xf0, 0, BIT19);
  }

  if (!EFI_ERROR (Status)) {
    Status = Modify (Pci, EfiPciIoWidthUint8, 0x37, 0, BIT7);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (UINTN Us = 0; Us < 50000; Us += 100) {
    UINT8   Mcu;
    UINT16  Empty;

    Status = R27Read (Pci, EfiPciIoWidthUint8, 0xd3, &Mcu);
    if (!EFI_ERROR (Status)) {
      Status = R27Read (Pci, EfiPciIoWidthUint16, 0xe2, &Empty);
    }

    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (((Mcu & 0x30) == 0x30) && ((Empty & 0x103) == 0x103)) {
      return R27Reset (Pci);
    }

    MicroSecondDelay (100);
  }

  return EFI_TIMEOUT;
}
