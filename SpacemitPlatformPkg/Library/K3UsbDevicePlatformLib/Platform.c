/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/Fdt.h>
#include <Library/BaseLib.h>
#include <Library/K3HardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiLib.h>
#include <Library/UsbDevicePlatformLib.h>

// Port A high-speed device operation uses its dedicated UTMI PHY. The sequencing matches the
// BSD K3 UsbHcdInitDxe reference; the 24MHz divider matches the K3 PHY driver.
#define USB_CLOCK_CONTROL  0x5c
#define PORT_A_CLOCK       BIT4
#define PORT_A_RESETS      (BIT5 | BIT6 | BIT7)
#define PHY_RESET_MODE     0x04
#define PHY_PLL_READY      BIT0
#define PHY_PLL_DIVIDER    0x98
#define DWC3_GUCTL1        0xc11c
#define DWC3_GUSB2PHYCFG   0xc200
#define DWC3_GUSB3PIPECTL  0xc2c0
#define FORCE_USB2_CLOCK   BIT26

STATIC EFI_STATUS
ColdInit (
  UINTN        Base,
  UINTN        Apmu,
  UINTN        Phy,
  CONST UINTN  *Pipe
  )
{
  EFI_STATUS  Status;

  MmioAndThenOr32 (Apmu + USB_CLOCK_CONTROL, ~PORT_A_RESETS, PORT_A_CLOCK);
  MmioOr32 (Apmu + USB_CLOCK_CONTROL, PORT_A_RESETS);
  if ((MmioRead32 (Apmu + USB_CLOCK_CONTROL) & (PORT_A_CLOCK | PORT_A_RESETS)) !=
      (PORT_A_CLOCK | PORT_A_RESETS))
  {
    Status = EFI_DEVICE_ERROR;
    goto Fail;
  }

  MicroSecondDelay (200);
  // High-speed mode must park both orientation-specific SuperSpeed PHYs.
  for (UINTN I = 0; I < 2; I++) {
    MmioAndThenOr32 (Pipe[I] + 0x40, ~BIT9, BIT10);
  }

  MmioWrite32 (Phy + PHY_PLL_DIVIDER, 0xbec4);
  for (UINTN Elapsed = 0; ; Elapsed += 500) {
    if (MmioRead32 (Phy + PHY_RESET_MODE) & PHY_PLL_READY) {
      break;
    }

    if (Elapsed == 50000) {
      Status = EFI_TIMEOUT;
      goto Fail;
    }

    MicroSecondDelay (500);
  }

  MmioWrite32 (Phy + PHY_RESET_MODE, 0x60ef);
  MmioWrite32 (Phy + 0x34, 0x1c);
  MmioAnd32 (Phy + 0x18, ~BIT0);
  MmioAndThenOr32 (Phy + 0xa4, ~0xfU, 0xc | BIT4);
  MmioOr32 (Phy + 0x10, BIT2);
  // The DWC3 device core reset needs its clock before Fastboot starts it.
  MmioOr32 (Base + DWC3_GUCTL1, FORCE_USB2_CLOCK | BIT28 | BIT17);
  MmioAndThenOr32 (
    Base + DWC3_GUSB2PHYCFG,
    ~(BIT3 | BIT6 | BIT8 | (0xfU << 10) | BIT31),
    (9U << 10) | 7
    );
  MmioAnd32 (Base + DWC3_GUSB3PIPECTL, ~(BIT17 | BIT18 | BIT31));
  return EFI_SUCCESS;

Fail:
  // No DMA has been published. Park only Port A; preserve other USB ports.
  MmioAnd32 (Apmu + USB_CLOCK_CONTROL, ~PORT_A_RESETS);
  MmioAnd32 (Apmu + USB_CLOCK_CONTROL, ~PORT_A_CLOCK);
  Print (L"K3 USB Port A cold initialization failed: %r\r\n", Status);
  return Status;
}

STATIC BOOLEAN
PropertyString (
  CONST VOID   *Fdt,
  INT32        Node,
  CONST CHAR8  *Name,
  CONST CHAR8  *Value
  )
{
  INT32        Length;
  CONST CHAR8  *Text = FdtGetProp (Fdt, Node, Name, &Length);

  return Text && Length == (INT32)AsciiStrLen (Value) + 1 && !AsciiStrCmp (Text, Value);
}

EFI_STATUS
UsbDevicePlatformInit (
  UINTN  *Base
  )
{
  STATIC CONST UINT32  ResetIds[]    = { 19, 20, 51 };
  STATIC CONST CHAR8   *ResetNames[] = { "ahb", "vcc", "phy" };
  VOID                 *Fdt;
  INT32                Node     = -1;
  INT32                Selected = -1;
  INT32                Length;
  INT32                Provider;
  INT32                PhyNode;
  UINTN                Apmu;
  UINTN                Phy;
  UINTN                Pipe[2];
  EFI_STATUS           Status;
  CONST UINT32         *Clocks;
  CONST UINT32         *Resets;
  CONST UINT32         *Phys;
  CONST UINT32         *PhyClock;

  if (Base == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Fdt);
  if (EFI_ERROR (Status) || FdtCheckHeader (Fdt)) {
    return EFI_NOT_FOUND;
  }

  // Consume the same controller and PHY nodes as Linux. Port A is the
  // dedicated Pico ITX download connector; the other ports remain hosts.
  while ((Node = FdtNodeOffsetByCompatible (Fdt, Node, "spacemit,k3-dwc3")) >= 0) {
    UINTN  Address;
    if (!K3FdtEnabled (Fdt, Node)) {
      continue;
    }

    Status = K3FdtRegister (Fdt, Node, 0, 0x10000, &Address, NULL);
    if (EFI_ERROR (Status) || (Address != 0xcad00000)) {
      continue;
    }

    if (Selected >= 0) {
      return EFI_COMPROMISED_DATA;
    }

    Selected = Node;
    *Base    = Address;
  }

  if (Selected < 0) {
    return EFI_NOT_FOUND;
  }

  if (!PropertyString (Fdt, Selected, "dr_mode", "peripheral") &&
      !(PropertyString (Fdt, Selected, "dr_mode", "otg") &&
        PropertyString (Fdt, Selected, "role-switch-default-mode", "peripheral") &&
        FdtGetProp (Fdt, Selected, "usb-role-switch", &Length)))
  {
    return EFI_UNSUPPORTED;
  }

  if (!PropertyString (Fdt, Selected, "maximum-speed", "high-speed") ||
      !PropertyString (Fdt, Selected, "phy_type", "utmi"))
  {
    return EFI_UNSUPPORTED;
  }

  Clocks = FdtGetProp (Fdt, Selected, "clocks", &Length);
  if (!Clocks || (Length != 8) || (Fdt32ToCpu (Clocks[1]) != 31) ||
      (FdtStringListSearch (Fdt, Selected, "clock-names", "usbdrd30") != 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  Provider = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Clocks[0]));
  if ((Provider < 0) || !K3FdtEnabled (Fdt, Provider) ||
      (FdtStringListSearch (Fdt, Provider, "compatible", "spacemit,k3-syscon-apmu") < 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  Status = K3FdtRegister (Fdt, Provider, 0, 0x60, &Apmu, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Resets = FdtGetProp (Fdt, Selected, "resets", &Length);
  if (!Resets || (Length != 24)) {
    return EFI_COMPROMISED_DATA;
  }

  for (UINTN I = 0; I < ARRAY_SIZE (ResetIds); I++) {
    if ((Resets[2 * I] != Clocks[0]) || (Fdt32ToCpu (Resets[2 * I + 1]) != ResetIds[I]) ||
        (FdtStringListSearch (Fdt, Selected, "reset-names", ResetNames[I]) != (INT32)I))
    {
      return EFI_COMPROMISED_DATA;
    }
  }

  Phys = FdtGetProp (Fdt, Selected, "phys", &Length);
  if (!Phys || (Length != 8) ||
      (FdtStringListSearch (Fdt, Selected, "phy-names", "usb2-phy") != 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  PhyNode = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Phys));
  if ((PhyNode < 0) || !K3FdtEnabled (Fdt, PhyNode) ||
      (FdtStringListSearch (Fdt, PhyNode, "compatible", "spacemit,k3-usb2-phy") < 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  PhyClock = FdtGetProp (Fdt, PhyNode, "clocks", &Length);
  if (!PhyClock || (Length != 8) || (PhyClock[0] != Clocks[0]) || (PhyClock[1] != Clocks[1])) {
    return EFI_COMPROMISED_DATA;
  }

  Status = K3FdtRegister (Fdt, PhyNode, 0, 0x200, &Phy, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Apmu != 0xd4282800) || (Phy != 0xcad20000)) {
    return EFI_UNSUPPORTED;
  }

  if (FdtStringListSearch (Fdt, Selected, "phy-names", "usb3-phy") != 1) {
    return EFI_COMPROMISED_DATA;
  }

  PhyNode = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Phys[1]));
  if ((PhyNode < 0) || !K3FdtEnabled (Fdt, PhyNode) ||
      (FdtStringListSearch (Fdt, PhyNode, "compatible", "spacemit,k3-usb3-phy") < 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  for (UINTN I = 0; I < 2; I++) {
    Status = K3FdtRegister (Fdt, PhyNode, I, 0x200, &Pipe[I], NULL);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Pipe[I] != 0xcad30000 + I * 0x10000) {
      return EFI_UNSUPPORTED;
    }

    Status = K3MapMmio (Pipe[I], 0x200);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = K3MapMmio (Apmu, 0x60);
  if (!EFI_ERROR (Status)) {
    Status = K3MapMmio (*Base, 0x10000);
  }

  if (!EFI_ERROR (Status)) {
    Status = K3MapMmio (Phy, 0x200);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  // A BootROM USB download already configured this port. SPI boot must
  // establish the high-speed resources itself, before the DWC3 core reset.
  if ((MmioRead32 (Apmu + USB_CLOCK_CONTROL) & (PORT_A_CLOCK | PORT_A_RESETS)) !=
      (PORT_A_CLOCK | PORT_A_RESETS))
  {
    return ColdInit (*Base, Apmu, Phy, Pipe);
  }

  return EFI_SUCCESS;
}
