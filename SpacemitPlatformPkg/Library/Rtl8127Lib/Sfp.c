/* SPDX-License-Identifier: GPL-2.0-only */

/* Adapted from Linux drivers/net/ethernet/realtek/r8169_main.c,
   commit ee6f8ef68a77339e4c210784e941119fdae55dd5.
   Copyright (c) 2002 ShuChen <shuchen@realtek.com.tw>
   Copyright (c) 2003 - 2007 Francois Romieu <romieu@fr.zoreil.com>
   Additional contributions by the Linux r8169 authors.

   RTL8127 SFP register sequence follows r8127_sfp_init_10g() and the OCP
   accessors in the existing K3-enabled Linux r8169 driver. This is SFP
   SerDes preparation only; MAC reset, packet rings and DMA are separate. */
#include <Library/Rtl8127Lib.h>
#include <Library/TimerLib.h>

#define RTL_BAR          2
#define RTL_ID           0x812710ec
#define RTL_COMMAND      0x37
#define RTL_TX_CONFIG    0x40
#define RTL_REV_MASK     0x7cf00000
#define RTL_REV_8127A    0x6c900000
#define RTL_MAC_OCP      0xb0
#define RTL_PHY_OCP      0xb8
#define RTL_OCP_WRITE    BIT31
#define RTL_PHY_BMCR     0xa400
#define RTL_PHY_PDOWN    BIT11
#define RTL_MEDIA_MODE   0xd006
#define RTL_SDS_CONTROL  0x2350
#define RTL_SDS_ADDRESS  0x233a
#define RTL_SDS_DATA     0x233e

STATIC EFI_STATUS
Read (
  EFI_PCI_IO_PROTOCOL        *Pci,
  EFI_PCI_IO_PROTOCOL_WIDTH  Width,
  UINT32                     Offset,
  VOID                       *Value
  )
{
  return Pci->Mem.Read (Pci, Width, RTL_BAR, Offset, 1, Value);
}

STATIC EFI_STATUS
Write (
  EFI_PCI_IO_PROTOCOL        *Pci,
  EFI_PCI_IO_PROTOCOL_WIDTH  Width,
  UINT32                     Offset,
  UINT32                     Value
  )
{
  return Pci->Mem.Write (Pci, Width, RTL_BAR, Offset, 1, &Value);
}

STATIC EFI_STATUS
PhyAccess (
  EFI_PCI_IO_PROTOCOL  *Pci,
  UINT16               Register,
  BOOLEAN              Store,
  UINT16               *Value
  )
{
  EFI_STATUS  Status;
  UINT32      Command;
  UINT32      Result;

  if ((Register & 1) || (Value == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Command = (UINT32)Register << 15;
  if (Store) {
    Command |= RTL_OCP_WRITE | *Value;
  }

  Status = Write (Pci, EfiPciIoWidthUint32, RTL_PHY_OCP, Command);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (UINTN Attempt = 0; Attempt < 10; Attempt++) {
    MicroSecondDelay (25);
    Status = Read (Pci, EfiPciIoWidthUint32, RTL_PHY_OCP, &Result);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (((Result & RTL_OCP_WRITE) != 0) != Store) {
      if (!Store) {
        *Value = (UINT16)Result;
      }

      return EFI_SUCCESS;
    }
  }

  return EFI_TIMEOUT;
}

EFI_STATUS
Rtl8127InitializeSfp (
  EFI_PCI_IO_PROTOCOL  *Pci
  )
{
  EFI_STATUS  Status;
  UINT32      Id;
  UINT32      Revision;
  UINT32      Mode;
  UINT8       Command;
  UINT8       Control;
  UINT16      Value;
  STATIC CONST struct {
    UINT16    Register;
    UINT16    Value;
  } PhySettings[] = {
    { 0xc40a, 0x0000 },
    { 0xc466, 0x0003 },
    { 0xc808, 0x0000 },
    { 0xc80a, 0x0000 }
  };

  if (Pci == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = Pci->Pci.Read (Pci, EfiPciIoWidthUint32, 0, 1, &Id);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Id != RTL_ID) {
    return EFI_UNSUPPORTED;
  }

  Status = Read (Pci, EfiPciIoWidthUint32, RTL_TX_CONFIG, &Revision);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Revision & RTL_REV_MASK) != RTL_REV_8127A) {
    return EFI_UNSUPPORTED;
  }

  Status = Read (Pci, EfiPciIoWidthUint8, RTL_COMMAND, &Command);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Command & (BIT4 | BIT3 | BIT2)) != 0) {
    return EFI_NOT_READY;
  }

  Status = Write (Pci, EfiPciIoWidthUint32, RTL_MAC_OCP, RTL_MEDIA_MODE << 15);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Read (Pci, EfiPciIoWidthUint32, RTL_MAC_OCP, &Mode);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Mode & 0xff) != 7) {
    return EFI_UNSUPPORTED;
  }

  // A previous OS may have suspended the PHY before rebooting.
  // Resume it before configuring the SFP SerDes.
  Status = PhyAccess (Pci, RTL_PHY_BMCR, FALSE, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Value & RTL_PHY_PDOWN) {
    Value &= ~RTL_PHY_PDOWN;
    Status = PhyAccess (Pci, RTL_PHY_BMCR, TRUE, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    MicroSecondDelay (20000);
  }

  Status = Read (Pci, EfiPciIoWidthUint8, RTL_SDS_CONTROL, &Control);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Write (Pci, EfiPciIoWidthUint8, RTL_SDS_CONTROL, Control & ~BIT0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MicroSecondDelay (1);
  Status = Write (Pci, EfiPciIoWidthUint16, RTL_SDS_ADDRESS, 0x801f);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Write (Pci, EfiPciIoWidthUint8, RTL_SDS_CONTROL, Control | BIT0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MicroSecondDelay (10);
  Status = Write (Pci, EfiPciIoWidthUint16, RTL_SDS_ADDRESS, 0x801a);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Read (Pci, EfiPciIoWidthUint16, RTL_SDS_DATA, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Write (Pci, EfiPciIoWidthUint16, RTL_SDS_DATA, (Value & ~0x3003) | 0x1000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (UINTN Index = 0; Index < sizeof (PhySettings) / sizeof (PhySettings[0]); Index++) {
    Value  = PhySettings[Index].Value;
    Status = PhyAccess (Pci, PhySettings[Index].Register, TRUE, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = PhyAccess (Pci, 0xc804, FALSE, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Value = (Value & ~0x000f) | 0x000c;
  return PhyAccess (Pci, 0xc804, TRUE, &Value);
}
