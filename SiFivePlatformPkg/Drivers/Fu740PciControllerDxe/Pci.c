/* SPDX-License-Identifier: GPL-2.0-only */

/* FU740 PHY and link sequencing follows Linux pcie-fu740.c.
 * Copyright (C) 2019-2021 SiFive, Inc.
 * Paul Walmsley, Greentime Hu
 * Copyright (C) 2013 Kosagi
 * Board power/reset timing follows U-Boot pcie_dw_sifive.c.
 */
#include <Uefi.h>
#include <IndustryStandard/Pci.h>
#include <Guid/FdtHob.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/PlatformPciController.h>
#include <Protocol/PciHostBridgeResourceAllocation.h>
#include <Protocol/PciIo.h>

#define PCIE_PERST        0x000
#define PCIE_LTSSM        0x010
#define PCIE_PHY_RESET    0x018
#define PCIE_DEVICE_TYPE  0x708
#define PCIE_PHY_ADDR     0x860
#define PCIE_PHY_SELECT   0x880
#define PCIE_PHY_DATA     0x888
#define PCIE_PHY_WRITE    0x890
#define PCIE_PHY_ACK      0x898
#define PHY_STRIDE        0x040
#define PHY_RX_OVERRIDE   0x1008
#define PHY_RX_VALUE      (BIT0 | BIT5 | BIT9 | BIT10 | BIT11)
#define DBI_RO_WRITE      0x8bc
#define DBI_LINK_STATUS   0x72c
#define DBI_LINK_SPEED    0x80c
#define DBI_VERSION       0x8f8
#define ATU_OFFSET        0x300000
#define ATU_STRIDE        0x200
#define ATU_ENABLE        BIT31
#define ATU_LARGE_REGION  BIT13
#define PRCI_PCIE_AUX     0x14
#define PRCI_RESET_N      0x28
#define GPIO_INPUT_EN     0x04
#define GPIO_OUTPUT_EN    0x08
#define GPIO_OUTPUT_VAL   0x0c
#define GPIO_IOF_EN       0x38
#define GPIO_OUTPUT_XOR   0x40

#define BOARD_RESET_DELAY_US  100000
#define LINK_CONFIG_DELAY_US  100000

STATIC UINTN                         mDbi, mConfig, mMgmt, mPrci, mGpio;
STATIC UINT32                        mPowerPin, mResetPin, mVersion;
STATIC UINT32                        mMaxSpeed = 3;
STATIC RUYI_PCI_CONTROLLER_PROTOCOL  mController;
STATIC struct {
  ACPI_HID_DEVICE_PATH        Root;
  EFI_DEVICE_PATH_PROTOCOL    End;
} mPath = {
  {
    { ACPI_DEVICE_PATH,     ACPI_DP,                        { sizeof (ACPI_HID_DEVICE_PATH),     0 }
    }, EISA_PNP_ID (0x0a08), 0
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

typedef struct {
  UINT64    Cpu, Pci, Size;
  UINT32    Type;
} PCI_WINDOW;
STATIC PCI_WINDOW  mWindows[4];
STATIC UINTN       mWindowCount;

STATIC UINT64
Cells64 (
  CONST UINT32  *Cells
  )
{
  return LShiftU64 (Fdt32ToCpu (Cells[0]), 32) | Fdt32ToCpu (Cells[1]);
}

STATIC EFI_STATUS
Reg (
  CONST VOID  *Fdt,
  INT32       Node,
  UINTN       Index,
  UINTN       *Base,
  UINT64      *Size
  )
{
  INT32         Length;
  CONST UINT32  *Data = FdtGetProp (Fdt, Node, "reg", &Length);

  if (!Data || (Length <= 0) || (Length % 16) || (Index >= (UINTN)Length / 16)) {
    return EFI_COMPROMISED_DATA;
  }

  *Base = Cells64 (Data + Index * 4);
  *Size = Cells64 (Data + Index * 4 + 2);
  if (!*Size || (*Base > MAX_UINT64 - *Size)) {
    return EFI_COMPROMISED_DATA;
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Map (
  UINTN   Base,
  UINT64  Size
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;

  if ((Base | Size) & EFI_PAGE_MASK) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Base, Size, EFI_MEMORY_UC);
  if (EFI_ERROR (Status)) {
    Status = gDS->GetMemorySpaceDescriptor (Base, &Descriptor);
    if (EFI_ERROR (Status) || (Descriptor.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) ||
        (Descriptor.BaseAddress > Base) || (Descriptor.Length < Size) ||
        (Base - Descriptor.BaseAddress > Descriptor.Length - Size))
    {
      return EFI_UNSUPPORTED;
    }
  }

  return gDS->SetMemorySpaceAttributes (Base, Size, EFI_MEMORY_UC);
}

STATIC EFI_STATUS
Poll (
  UINTN   Address,
  UINT32  Mask,
  UINT32  Value,
  UINTN   TimeoutUs
  )
{
  for (UINTN Time = 0; Time < TimeoutUs; Time += 10) {
    if ((MmioRead32 (Address) & Mask) == Value) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

STATIC VOID
GpioSet (
  UINT32   Pin,
  BOOLEAN  High
  )
{
  UINT32  Mask = 1U << Pin;

  MmioAnd32 (mGpio + GPIO_IOF_EN, ~Mask);
  MmioAnd32 (mGpio + GPIO_OUTPUT_XOR, ~Mask);
  MmioAnd32 (mGpio + GPIO_INPUT_EN, ~Mask);
  MmioAndThenOr32 (mGpio + GPIO_OUTPUT_VAL, ~Mask, High ? Mask : 0);
  MmioOr32 (mGpio + GPIO_OUTPUT_EN, Mask);
}

STATIC EFI_STATUS
PhyWrite (
  UINTN   Phy,
  UINT16  Address,
  UINT16  Value
  )
{
  UINTN       Base = mMgmt + Phy * PHY_STRIDE;
  EFI_STATUS  Status;

  MmioWrite32 (Base + PCIE_PHY_ADDR, Address);
  MmioWrite32 (Base + PCIE_PHY_DATA, Value);
  MmioWrite32 (Base + PCIE_PHY_WRITE, 1);
  Status = Poll (Base + PCIE_PHY_ACK, BIT0, BIT0, 5000);
  MmioWrite32 (Base + PCIE_PHY_WRITE, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Poll (Base + PCIE_PHY_ACK, BIT0, 0, 5000);
}

STATIC BOOLEAN
LinkUp (
  VOID
  )
{
  return (MmioRead32 (mDbi + DBI_LINK_STATUS) & (BIT4 | BIT29)) == BIT4;
}

STATIC EFI_STATUS
WaitLink (
  VOID
  )
{
  for (UINTN Retry = 0; Retry < 100; Retry++) {
    if (LinkUp ()) {
      // Match Linux's post-training delay before configuration requests.
      if (mMaxSpeed > 2) {
        MicroSecondDelay (LINK_CONFIG_DELAY_US);
      }

      return LinkUp () ? EFI_SUCCESS : EFI_TIMEOUT;
    }

    MicroSecondDelay (10000);
  }

  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
Window (
  UINTN   Index,
  UINT32  Type,
  UINT64  Cpu,
  UINT64  Pci,
  UINT64  Size
  )
{
  UINTN   Base = mDbi + ATU_OFFSET + Index * ATU_STRIDE;
  UINT64  Limit;

  if (!Size || (Cpu > MAX_UINT64 - (Size - 1)) ||
      (Pci > MAX_UINT64 - (Size - 1)) || ((Cpu | Pci | Size) & 0xfff))
  {
    return EFI_INVALID_PARAMETER;
  }

  Limit = Cpu + Size - 1;
  if ((Cpu >> 32) != (Limit >> 32)) {
    if (mVersion < 0x3436302a) {
      return EFI_UNSUPPORTED;
    }

    Type |= ATU_LARGE_REGION;
  }

  MmioWrite32 (Base + 4, 0);
  MmioWrite32 (Base + 8, Cpu);
  MmioWrite32 (Base + 12, Cpu >> 32);
  MmioWrite32 (Base + 16, Limit);
  MmioWrite32 (Base + 20, Pci);
  MmioWrite32 (Base + 24, Pci >> 32);
  if (mVersion >= 0x3436302a) {
    MmioWrite32 (Base + 32, Limit >> 32);
  }

  MmioWrite32 (Base, Type);
  MemoryFence ();
  MmioWrite32 (Base + 4, ATU_ENABLE);
  return Poll (Base + 4, ATU_ENABLE, ATU_ENABLE, 1000);
}

STATIC EFI_STATUS EFIAPI
Config (
  UINT16   Segment,
  UINT8    Bus,
  UINT8    Device,
  UINT8    Function,
  UINT16   Register,
  UINTN    Width,
  BOOLEAN  Write,
  UINT32   *Value
  )
{
  EFI_STATUS  Status = EFI_SUCCESS;
  EFI_TPL     Tpl;
  UINTN       Address;

  if (!Value || Segment || (Device > 31) || (Function > 7) ||
      ((Width != 1) && (Width != 2) && (Width != 4)) ||
      (Register >= 4096) || (Register & (Width - 1)) || (Width > 4096U - Register))
  {
    return EFI_INVALID_PARAMETER;
  }

  if ((!Bus && (Device || Function)) || ((Bus == 1) && Device)) {
    return EFI_NOT_FOUND;
  }

  if (Bus && !LinkUp ()) {
    return EFI_NOT_READY;
  }

  Address = (Bus ? mConfig : mDbi) + Register;
  Tpl     = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  if (Bus) {
    Status = Window (
               0,
               Bus == 1 ? 4 : 5,
               mConfig,
               ((UINT32)Bus << 24) | ((UINT32)Device << 19) | ((UINT32)Function << 16),
               4096
               );
  }

  if (!EFI_ERROR (Status)) {
    if (Write) {
      if (Width == 1) {
        MmioWrite8 (Address, *Value);
      } else if (Width == 2) {
        MmioWrite16 (Address, *Value);
      } else {
        MmioWrite32 (Address, *Value);
      }
    } else {
      if (Width == 1) {
        *Value = MmioRead8 (Address);
      } else if (Width == 2) {
        *Value = MmioRead16 (Address);
      } else {
        *Value = MmioRead32 (Address);
      }
    }
  }

  gBS->RestoreTPL (Tpl);
  return Status;
}

STATIC EFI_STATUS
Start (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Cap;
  UINT32      Original, Value;

  // Match U-Boot's board power/reset sequence, including a powered-off
  // interval on re-entry. PCIe_PWREN also supplies the Unmatched M.2 slots.
  MmioWrite32 (mMgmt + PCIE_LTSSM, 0);
  GpioSet (mResetPin, FALSE);
  MmioWrite32 (mMgmt + PCIE_PERST, 0);
  GpioSet (mPowerPin, FALSE);
  MicroSecondDelay (BOARD_RESET_DELAY_US);
  GpioSet (mPowerPin, TRUE);
  MicroSecondDelay (BOARD_RESET_DELAY_US);
  MmioWrite32 (mMgmt + PCIE_PERST, 1);
  GpioSet (mResetPin, TRUE);
  MicroSecondDelay (BOARD_RESET_DELAY_US);
  MmioOr32 (mPrci + PRCI_PCIE_AUX, BIT0);
  MmioWrite32 (mMgmt + PCIE_PHY_RESET, 1);
  MmioOr32 (mPrci + PRCI_RESET_N, BIT4);
  for (UINTN Phy = 0; Phy < 2; Phy++) {
    MmioWrite32 (mMgmt + Phy * PHY_STRIDE + PCIE_PHY_SELECT, 1);
  }

  MicroSecondDelay (1);
  for (UINTN Phy = 0; Phy < 2; Phy++) {
    for (UINTN Lane = 0; Lane < 4; Lane++) {
      Status = PhyWrite (Phy, PHY_RX_OVERRIDE + Lane * 0x100, PHY_RX_VALUE);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  MmioAnd32 (mPrci + PRCI_PCIE_AUX, ~BIT0);
  MmioWrite32 (mMgmt + PCIE_PHY_RESET, 0);
  MmioOr32 (mPrci + PRCI_PCIE_AUX, BIT0);
  MmioWrite32 (mMgmt + PCIE_DEVICE_TYPE, 4);
  mVersion = MmioRead32 (mDbi + DBI_VERSION);

  // Configure the root port while LTSSM is stopped.
  MmioWrite32 (mMgmt + PCIE_LTSSM, 0);
  MmioOr32 (mDbi + DBI_RO_WRITE, BIT0);
  MmioWrite32 (mDbi + 0x10, 0);
  MmioWrite32 (mDbi + 0x14, 0);
  MmioWrite16 (mDbi + (PCI_CLASSCODE_OFFSET + 1), 0x0604);
  MmioWrite32 (mDbi + PCI_BRIDGE_PRIMARY_BUS_REGISTER_OFFSET, 0x00ff0100);
  MmioOr16 (mDbi + PCI_COMMAND_OFFSET, EFI_PCI_COMMAND_MEMORY_SPACE | EFI_PCI_COMMAND_BUS_MASTER);
  Cap = MmioRead8 (mDbi + PCI_CAPBILITY_POINTER_OFFSET) & ~3U;
  for (UINTN Retry = 0; Retry < 48; Retry++) {
    if ((Cap < 0x40) || (Cap > 0xfc)) {
      return EFI_COMPROMISED_DATA;
    }

    if (MmioRead8 (mDbi + Cap) == EFI_PCI_CAPABILITY_ID_PCIEXP) {
      break;
    }

    Cap = MmioRead8 (mDbi + Cap + 1) & ~3U;
  }

  if ((Cap > 0xcc) || (MmioRead8 (mDbi + Cap) != EFI_PCI_CAPABILITY_ID_PCIEXP)) {
    return EFI_UNSUPPORTED;
  }

  Original = MmioRead32 (mDbi + Cap + 0x0c);
  // Earlier firmware may leave this writable speed field at 0xf.
  // FU740 supports Gen3; honor a lower device-tree limit as well.
  Value = Original & 0xf;
  if (!Value || (Value > mMaxSpeed)) {
    Original = (Original & ~0xfU) | mMaxSpeed;
  }

  MmioWrite32 (mDbi + Cap + 0x0c, (Original & ~0xfU) | 1);
  MmioWrite32 (mMgmt + PCIE_LTSSM, 1);
  Status = WaitLink ();
  if (!EFI_ERROR (Status) && ((Original & 0xf) > 1)) {
    MmioWrite32 (mDbi + Cap + 0x0c, Original);
    MmioOr32 (mDbi + DBI_LINK_SPEED, BIT17);
    Status = WaitLink ();
  }

  MmioAnd32 (mDbi + DBI_RO_WRITE, ~BIT0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Value = MmioRead16 (mDbi + Cap + 0x12);
  DEBUG ((DEBUG_INFO, "FU740 PCIe: Gen%u x%u, DesignWare %08x\n", Value & 0xf, (Value >> 4) & 0x3f, mVersion));
  for (UINTN Index = 0; Index < mWindowCount; Index++) {
    PCI_WINDOW  *Range = &mWindows[Index];
    Status = Window (Index + 1, Range->Type, Range->Cpu, Range->Pci, Range->Size);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

STATIC VOID
Empty (
  PCI_ROOT_BRIDGE_APERTURE  *Aperture
  )
{
  Aperture->Base        = MAX_UINT64;
  Aperture->Limit       = 0;
  Aperture->Translation = 0;
}

STATIC EFI_STATUS
Parse (
  CONST VOID  *Fdt
  )
{
  INT32            Node, Parent, Length, Index;
  UINT64           Size;
  CONST UINT32     *Data;
  EFI_STATUS       Status;
  PCI_ROOT_BRIDGE  *Bridge = &mController.RootBridge;
  UINTN            Bases[3];
  CONST CHAR8      *Names[]  = { "dbi", "config", "mgmt" };
  CONST UINT64     Minimum[] = { ATU_OFFSET + 0x1000, 0x1000, 0x1000 };

  Node = FdtNodeOffsetByCompatible (Fdt, -1, "sifive,fu740-pcie");
  if (Node < 0) {
    return EFI_NOT_FOUND;
  }

  CONST CHAR8  *State = FdtGetProp (Fdt, Node, "status", &Length);

  if (State && !(((Length == 5) && !CompareMem (State, "okay", 5)) ||
                 ((Length == 3) && !CompareMem (State, "ok", 3))))
  {
    return EFI_NOT_FOUND;
  }

  Data = FdtGetProp (Fdt, Node, "bus-range", &Length);
  if (!Data || (Length != 8) || Data[0] || (Fdt32ToCpu (Data[1]) != 255)) {
    return EFI_UNSUPPORTED;
  }

  Data = FdtGetProp (Fdt, Node, "num-lanes", &Length);
  if (!Data || (Length != 4) || (Fdt32ToCpu (*Data) != 8)) {
    return EFI_UNSUPPORTED;
  }

  if (FdtGetProp (Fdt, Node, "dma-coherent", NULL) == NULL) {
    return EFI_UNSUPPORTED;
  }

  for (UINTN I = 0; I < 3; I++) {
    Index = FdtStringListSearch (Fdt, Node, "reg-names", Names[I]);
    if (Index < 0) {
      return EFI_COMPROMISED_DATA;
    }

    Status = Reg (Fdt, Node, Index, &Bases[I], &Size);
    if (EFI_ERROR (Status) || (Size < Minimum[I])) {
      return EFI_COMPROMISED_DATA;
    }

    Status = Map (Bases[I], Minimum[I]);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  mDbi    = Bases[0];
  mConfig = Bases[1];
  mMgmt   = Bases[2];
  Data    = FdtGetProp (Fdt, Node, "max-link-speed", &Length);
  if (Data) {
    if ((Length != 4) || !Fdt32ToCpu (*Data) || (Fdt32ToCpu (*Data) > 3)) {
      return EFI_UNSUPPORTED;
    }

    mMaxSpeed = Fdt32ToCpu (*Data);
  }

  Data = FdtGetProp (Fdt, Node, "clocks", &Length);
  if (!Data || (Length != 8) || (Fdt32ToCpu (Data[1]) != 8)) {
    return EFI_UNSUPPORTED;
  }

  Parent = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Data[0]));
  if ((Parent < 0) || (FdtStringListSearch (Fdt, Parent, "compatible", "sifive,fu740-c000-prci") < 0)) {
    return EFI_UNSUPPORTED;
  }

  Data = FdtGetProp (Fdt, Node, "resets", &Length);
  if (!Data || (Length != 8) || (Fdt32ToCpu (Data[1]) != 4) ||
      (FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Data[0])) != Parent))
  {
    return EFI_UNSUPPORTED;
  }

  Status = Reg (Fdt, Parent, 0, &mPrci, &Size);
  if (EFI_ERROR (Status) || (Size < 0x1000)) {
    return EFI_COMPROMISED_DATA;
  }

  Status = Map (mPrci, 0x1000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CONST CHAR8  *Pins[] = { "pwren-gpios", "reset-gpios" };

  for (UINTN I = 0; I < 2; I++) {
    UINTN  Base;
    Data = FdtGetProp (Fdt, Node, Pins[I], &Length);
    if (!Data || (Length != 12) || (Fdt32ToCpu (Data[1]) >= 16) || Data[2]) {
      return EFI_UNSUPPORTED;
    }

    if (I == 0) {
      mPowerPin = Fdt32ToCpu (Data[1]);
    } else {
      mResetPin = Fdt32ToCpu (Data[1]);
    }

    Parent = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Data[0]));
    Status = Reg (Fdt, Parent, 0, &Base, &Size);
    if (EFI_ERROR (Status) || (Size < 0x1000) || (I && (Base != mGpio))) {
      return EFI_UNSUPPORTED;
    }

    mGpio = Base;
  }

  Status = Map (mGpio, 0x1000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Bridge->Segment              = 0;
  Bridge->Supports             = Bridge->Attributes = EFI_PCI_IO_ATTRIBUTE_IO | EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
  Bridge->Supports            |= EFI_PCI_ATTRIBUTE_DUAL_ADDRESS_CYCLE;
  Bridge->DmaAbove4G           = TRUE;
  Bridge->AllocationAttributes = EFI_PCI_HOST_BRIDGE_MEM64_DECODE;
  Bridge->Bus.Base             = 0;
  Bridge->Bus.Limit            = 255;
  Bridge->DevicePath           = (VOID *)&mPath;
  Empty (&Bridge->Io);
  Empty (&Bridge->Mem);
  Empty (&Bridge->MemAbove4G);
  Empty (&Bridge->PMem);
  Empty (&Bridge->PMemAbove4G);
  Data = FdtGetProp (Fdt, Node, "ranges", &Length);
  if (!Data || (Length <= 0) || Length % 28 || (Length > sizeof (mWindows) / sizeof (*mWindows) * 28)) {
    return EFI_UNSUPPORTED;
  }

  for (UINTN I = 0; I < (UINTN)Length / 28; I++, Data += 7) {
    PCI_ROOT_BRIDGE_APERTURE  *Aperture;
    PCI_WINDOW                *Range = &mWindows[mWindowCount++];
    UINT32                    Flags  = Fdt32ToCpu (Data[0]);
    Range->Pci  = Cells64 (Data + 1);
    Range->Cpu  = Cells64 (Data + 3);
    Range->Size = Cells64 (Data + 5);
    if (!Range->Size || (Range->Pci > MAX_UINT64 - Range->Size) || (Range->Cpu > MAX_UINT64 - Range->Size)) {
      return EFI_COMPROMISED_DATA;
    }

    UINT32  Space = (Flags >> 24) & 3;
    if (!Space || ((Range->Cpu | Range->Pci | Range->Size) & EFI_PAGE_MASK) ||
        ((Space == 1) && (Flags & BIT30)) ||
        ((Space != 3) && (Range->Pci + Range->Size - 1 > MAX_UINT32)) ||
        ((Space == 3) && (!(Flags & BIT30) || (Range->Pci < SIZE_4GB))))
    {
      return EFI_UNSUPPORTED;
    }

    Range->Type = ((Flags >> 24) & 3) == 1 ? 2 : 0;
    Aperture    = Range->Type == 2 ? &Bridge->Io :
                  (Flags & BIT30) ? &Bridge->PMemAbove4G : &Bridge->Mem;
    if (Aperture->Base != MAX_UINT64) {
      if ((Aperture->Limit + 1 != Range->Pci) || (Aperture->Translation != Range->Pci - Range->Cpu)) {
        return EFI_UNSUPPORTED;
      }

      Aperture->Limit += Range->Size;
    } else {
      Aperture->Base        = Range->Pci;
      Aperture->Limit       = Range->Pci + Range->Size - 1;
      Aperture->Translation = Range->Pci - Range->Cpu;
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
Fu740PciEntry (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  VOID        *Hob = GetFirstGuidHob (&gFdtHobGuid);
  EFI_STATUS  Status;
  EFI_HANDLE  Handle = NULL;
  CONST VOID  *Fdt;

  if (!Hob) {
    return EFI_NOT_FOUND;
  }

  Fdt = (CONST VOID *)(UINTN)*(UINT64 *)GET_GUID_HOB_DATA (Hob);
  if (FdtCheckHeader (Fdt)) {
    return EFI_COMPROMISED_DATA;
  }

  Status = Parse (Fdt);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Start ();
  if (EFI_ERROR (Status)) {
    MmioWrite32 (mMgmt + PCIE_LTSSM, 0);
    MmioAnd32 (mDbi + DBI_RO_WRITE, ~BIT0);
    GpioSet (mResetPin, FALSE);
    DEBUG ((DEBUG_ERROR, "FU740 PCIe initialization: %r\n", Status));
    return Status;
  }

  mController.Revision = 1;
  mController.Config   = Config;
  return gBS->InstallProtocolInterface (&Handle, &gRuyiPciControllerProtocolGuid, EFI_NATIVE_INTERFACE, &mController);
}
