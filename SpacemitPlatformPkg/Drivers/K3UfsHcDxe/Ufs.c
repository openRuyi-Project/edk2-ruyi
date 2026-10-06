/** SPDX-License-Identifier: BSD-2-Clause-Patent */

/* Native K3 resources and standard UFS host-controller protocols.
 * PHY/UniPro register recipe adapted from SpacemiT's BSD UfsHcDxe,
 * Copyright (c) 2025 SpacemiT Co., Ltd. All rights reserved.
 */
#include <PiDxe.h>
#include <IndustryStandard/UfsHci.h>
#include <Guid/RiscVSecHobData.h>
#include <Protocol/DevicePath.h>
#include <Protocol/Clock.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsHostControllerPlatform.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DmaLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/K3HardwareLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define UFS_UPMS              BIT4
#define UFS_PHY               0x1b00
#define UFS_ATOP              0x1c00
#define UFS_MPHY_RESET        (UFS_PHY + 0x00)
#define UFS_MPHY_POWER        (UFS_PHY + 0x04)
#define UFS_MPHY_BACKDOOR     (UFS_PHY + 0x08)
#define UFS_DEVICE_IO         (UFS_PHY + 0x0c)
#define UFS_MPHY_PLL_LOCK     BIT31
#define UFS_PLL_TIMEOUT_US    10000
#define UFS_HCS_POWER_MASK    (7U << 8)
#define UFS_HCS_POWER_LOCAL   BIT8
#define UFS_FAST_MODE         0x11
#define UFS_ACLK_HZ           409600000U
#define UFS_POWER_TIMEOUT_US  1000000
#define UFS_PHY_STEP_US       1000
#define UFS_PHY_BACKDOOR_US   20
#define UFS_PHY_SETTLE_US     5000
#define UFS_POWER_SETTLE_US   10000

// UniPro attributes used when leaving the initial PWM link.
#define PA_ACTIVETXDATALANES     0x1560
#define PA_CONNECTEDTXDATALANES  0x1561
#define PA_TXGEAR                0x1568
#define PA_TXTERMINATION         0x1569
#define PA_HSSERIES              0x156a
#define PA_PWRMODE               0x1571
#define PA_ACTIVERXDATALANES     0x1580
#define PA_CONNECTEDRXDATALANES  0x1581
#define PA_RXGEAR                0x1583
#define PA_RXTERMINATION         0x1584
#define PA_MAXRXHSGEAR           0x1587
#define ANA_HSGEAR_CTRL          0x00c1
#define TX_LCC_ENABLE            0x002c
#define TX_MIN_ACTIVATETIME      0x0033

STATIC UINTN                               mBase;
STATIC UINTN                               mSize;
STATIC SPACEMIT_CLOCK_PROTOCOL             *mClock;
STATIC UINT32                              mAxiClock, mRefClock, mReset;
STATIC BOOLEAN                             mAxiEnabled;
STATIC BOOLEAN                             mRefEnabled;
STATIC EFI_HANDLE                          mHandle;
STATIC EDKII_UFS_HOST_CONTROLLER_PROTOCOL  mHc;
STATIC EFI_STATUS
Wait32 (
  UINTN   Address,
  UINT32  Mask,
  UINT32  Value,
  UINTN   Budget
  )
{
  for (UINTN Us = 0; Us < Budget; Us += 10) {
    if ((MmioRead32 (Address) & Mask) == Value) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
EFIAPI
Bar (
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  UINTN                               *Base
  )
{
  if ((This != &mHc) || !Base) {
    return EFI_INVALID_PARAMETER;
  }

  *Base = mBase;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
Allocate (
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  EFI_ALLOCATE_TYPE                   Type,
  EFI_MEMORY_TYPE                     MemoryType,
  UINTN                               Pages,
  VOID                                **Host,
  UINT64                              Attributes
  )
{
  if (This != &mHc) {
    return EFI_INVALID_PARAMETER;
  }

  if (Attributes) {
    return EFI_UNSUPPORTED;
  }

  return DmaAllocateBuffer (MemoryType, Pages, Host);
}

STATIC EFI_STATUS
EFIAPI
Free (
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  UINTN                               Pages,
  VOID                                *Host
  )
{
  return This == &mHc ? DmaFreeBuffer (Pages, Host) : EFI_INVALID_PARAMETER;
}

STATIC EFI_STATUS
EFIAPI
Map (
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL   *This,
  EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation,
  VOID                                 *Host,
  UINTN                                *Bytes,
  EFI_PHYSICAL_ADDRESS                 *Device,
  VOID                                 **Mapping
  )
{
  if ((This != &mHc) || (Operation >= EdkiiUfsHcOperationMaximum)) {
    return EFI_INVALID_PARAMETER;
  }

  return DmaMap ((DMA_MAP_OPERATION)Operation, Host, Bytes, Device, Mapping);
}

STATIC EFI_STATUS
EFIAPI
Unmap (
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  VOID                                *Mapping
  )
{
  return This == &mHc ? DmaUnmap (Mapping) : EFI_INVALID_PARAMETER;
}

STATIC EFI_STATUS
EFIAPI
Flush (
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This
  )
{
  if (This != &mHc) {
    return EFI_INVALID_PARAMETER;
  }

  MemoryFence ();
  (VOID)MmioRead32 (mBase + UFS_HC_STATUS_OFFSET);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Access (
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  UINT64                                    Offset,
  UINTN                                     Count,
  VOID                                      *Buffer,
  BOOLEAN                                   Write
  )
{
  if ((This != &mHc) || (Width >= EfiUfsHcWidthMaximum) || (!Buffer && Count)) {
    return EFI_INVALID_PARAMETER;
  }

  UINTN  Bytes = (UINTN)1 << Width;

  if ((Offset & (Bytes - 1)) || (Offset > mSize) || (Count > (mSize - Offset) / Bytes)) {
    return EFI_INVALID_PARAMETER;
  }

  for (UINTN I = 0; I < Count; I++) {
    UINTN   A = mBase + (UINTN)Offset + I * Bytes;
    UINT64  V = 0;
    if (Write) {
      CopyMem (&V, (UINT8 *)Buffer + I * Bytes, Bytes);
    }

    switch (Width) {
      case EfiUfsHcWidthUint8: if (Write) {
          MmioWrite8 (A, (UINT8)V);
      } else {
          V = MmioRead8 (A);
      }

        break;
      case EfiUfsHcWidthUint16: if (Write) {
          MmioWrite16 (A, (UINT16)V);
      } else {
          V = MmioRead16 (A);
      }

        break;
      case EfiUfsHcWidthUint32: if (Write) {
          MmioWrite32 (A, (UINT32)V);
      } else {
          V = MmioRead32 (A);
      }

        break;
      case EfiUfsHcWidthUint64: if (Write) {
          MmioWrite64 (A, V);
      } else {
          V = MmioRead64 (A);
      }

        break;
      default: return EFI_INVALID_PARAMETER;
    }

    if (!Write) {
      CopyMem ((UINT8 *)Buffer + I * Bytes, &V, Bytes);
    }
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
Read (
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  UINT64                                    Offset,
  UINTN                                     Count,
  VOID                                      *Buffer
  )
{
  return Access (This, Width, Offset, Count, Buffer, FALSE);
}

STATIC EFI_STATUS
EFIAPI
Write (
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  UINT64                                    Offset,
  UINTN                                     Count,
  VOID                                      *Buffer
  )
{
  return Access (This, Width, Offset, Count, Buffer, TRUE);
}

STATIC EFI_STATUS
DmeSet (
  EDKII_UFS_HC_DRIVER_INTERFACE  *Driver,
  UINT16                         Attribute,
  UINT16                         Selector,
  UINT32                         Value
  )
{
  EDKII_UIC_COMMAND  Cmd    = { UfsUicDmeSet, ((UINT32)Attribute << 16) | Selector, 0, Value };
  EFI_STATUS         Status = Driver->UfsExecUicCommand (Driver, &Cmd);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "K3 UFS DME_SET %04x/%u failed: %r\n", Attribute, Selector, Status));
  }

  return Status;
}

STATIC EFI_STATUS
DmeGet (
  EDKII_UFS_HC_DRIVER_INTERFACE  *Driver,
  UINT16                         Attribute,
  BOOLEAN                        Peer,
  UINT32                         *Value
  )
{
  EDKII_UIC_COMMAND  Cmd = {
    Peer ? UfsUicDmePeerGet : UfsUicDmeGet, (UINT32)Attribute << 16, 0, 0
  };
  EFI_STATUS         Status = Driver->UfsExecUicCommand (Driver, &Cmd);

  if (!EFI_ERROR (Status)) {
    *Value = Cmd.Arg3;
  } else {
    DEBUG ((
      DEBUG_ERROR,
      "K3 UFS DME_%aGET %04x failed: %r\n",
      Peer ? "PEER_" : "",
      Attribute,
      Status
      ));
  }

  return Status;
}

STATIC EFI_STATUS
ChangePowerMode (
  EDKII_UFS_HC_DRIVER_INTERFACE  *Driver,
  UINT32                         Mode
  )
{
  // UIC command completion alone does not complete a power transition.
  MmioWrite32 (mBase + UFS_HC_IS_OFFSET, UFS_UPMS);
  EFI_STATUS  Status = DmeSet (Driver, PA_PWRMODE, 0, Mode);

  if (!EFI_ERROR (Status)) {
    Status = Wait32 (mBase + UFS_HC_IS_OFFSET, UFS_UPMS, UFS_UPMS, UFS_POWER_TIMEOUT_US);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "K3 UFS power mode %02x: %r, IS=%08x HCS=%08x\n",
      Mode,
      Status,
      MmioRead32 (mBase + UFS_HC_IS_OFFSET),
      MmioRead32 (mBase + UFS_HC_STATUS_OFFSET)
      ));
    return Status;
  }

  UINT32  Result = MmioRead32 (mBase + UFS_HC_STATUS_OFFSET) & UFS_HCS_POWER_MASK;

  MmioWrite32 (mBase + UFS_HC_IS_OFFSET, UFS_UPMS);
  return Result == UFS_HCS_POWER_LOCAL ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS
PrepareHighSpeed (
  EDKII_UFS_HC_DRIVER_INTERFACE  *Driver
  )
{
  EFI_STATUS  Status;

  // K3 link-startup preparation: disable lane credit control and remove
  // the TX activation delay before switching the analog gear/rate.
  MicroSecondDelay (UFS_PHY_SETTLE_US);
  for (UINT16 Lane = 0; Lane < 2; Lane++) {
    Status = DmeSet (Driver, TX_LCC_ENABLE, Lane, 0);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    MicroSecondDelay (UFS_PHY_STEP_US);
  }

  for (UINT16 Lane = 0; Lane < 2; Lane++) {
    Status = DmeSet (Driver, TX_MIN_ACTIVATETIME, Lane, 0);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    MicroSecondDelay (Lane == 0 ? UFS_PHY_STEP_US : UFS_POWER_SETTLE_US);
  }

  Status = DmeSet (Driver, ANA_HSGEAR_CTRL, 0, 0x25);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MicroSecondDelay (UFS_POWER_SETTLE_US);
  return Wait32 (mBase + UFS_MPHY_POWER, UFS_MPHY_PLL_LOCK, UFS_MPHY_PLL_LOCK, UFS_PLL_TIMEOUT_US);
}

STATIC EFI_STATUS
HighSpeed (
  EDKII_UFS_HC_DRIVER_INTERFACE  *Driver
  )
{
  UINT32      RxGear, TxGear, RxLanes, TxLanes, Value;
  EFI_STATUS  Status;

  Status = PrepareHighSpeed (Driver);
  if (!EFI_ERROR (Status)) {
    Status = DmeGet (Driver, PA_MAXRXHSGEAR, FALSE, &RxGear);
  }

  if (!EFI_ERROR (Status)) {
    Status = DmeGet (Driver, PA_MAXRXHSGEAR, TRUE, &TxGear);
  }

  if (!EFI_ERROR (Status)) {
    Status = DmeGet (Driver, PA_CONNECTEDRXDATALANES, FALSE, &RxLanes);
  }

  if (!EFI_ERROR (Status)) {
    Status = DmeGet (Driver, PA_CONNECTEDTXDATALANES, FALSE, &TxLanes);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!RxGear || !TxGear || !RxLanes || !TxLanes) {
    return EFI_UNSUPPORTED;
  }

  // K3 supports at most HS-G3, two lanes and Rate B.
  RxGear  = MIN (RxGear, 3);
  TxGear  = MIN (TxGear, 3);
  RxLanes = MIN (RxLanes, 2);
  TxLanes = MIN (TxLanes, 2);
  CONST struct {
    UINT16    Attr;
    UINT32    Value;
  } Settings[] = {
    { PA_RXGEAR,
      RxGear },
    { PA_ACTIVERXDATALANES,
      RxLanes },
    { PA_RXTERMINATION,
      1 },
    { PA_TXGEAR,
      TxGear },
    { PA_ACTIVETXDATALANES,
      TxLanes },
    { PA_TXTERMINATION,
      1 },
    { PA_HSSERIES,
      2 },
    // UniPro default protection, replay and AFC timeouts, TC0 and TC1.
    { 0x15b0,
      8191 },
    { 0x15b1,
      65535 },
    { 0x15b2,
      32767 },
    { 0x15b3,
      8191 },
    { 0x15b4,
      65535 },
    { 0x15b5,
      32767 },
    { 0xd041,
      8191 },
    { 0xd042,
      65535 },
    { 0xd043,
      32767 }
  };

  for (UINTN I = 0; I < ARRAY_SIZE (Settings); I++) {
    Status = DmeSet (Driver, Settings[I].Attr, 0, Settings[I].Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  // Request FAST directly after the K3 analog preparation. Complete the
  // power transition and allow the PHY to settle before issuing queries.
  Status = ChangePowerMode (Driver, UFS_FAST_MODE);
  if (!EFI_ERROR (Status)) {
    MicroSecondDelay (UFS_POWER_SETTLE_US);
    Status = Wait32 (mBase + UFS_MPHY_POWER, UFS_MPHY_PLL_LOCK, UFS_MPHY_PLL_LOCK, UFS_PLL_TIMEOUT_US);
  }

  if (!EFI_ERROR (Status)) {
    MicroSecondDelay (UFS_PHY_SETTLE_US);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  CONST struct {
    UINT16    Attr;
    UINT32    Value;
  } Expected[] = {
    { PA_RXGEAR,
      RxGear },
    { PA_TXGEAR,
      TxGear },
    { PA_ACTIVERXDATALANES,
      RxLanes },
    { PA_ACTIVETXDATALANES,
      TxLanes },
    { PA_HSSERIES,
      2 },
    { PA_PWRMODE,
      UFS_FAST_MODE }
  };

  for (UINTN I = 0; I < ARRAY_SIZE (Expected); I++) {
    Status = DmeGet (Driver, Expected[I].Attr, FALSE, &Value);
    if (EFI_ERROR (Status) || (Value != Expected[I].Value)) {
      return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
    }
  }

  DEBUG ((
    DEBUG_INFO,
    "K3 UFS: HS-G%u/G%u, lanes %u/%u, Rate B\n",
    RxGear,
    TxGear,
    RxLanes,
    TxLanes
    ));
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Phy (
  EDKII_UFS_HC_DRIVER_INTERFACE  *Driver
  )
{
  STATIC CONST struct {
    UINT16    Attr;
    UINT16    Sel;
    UINT32    Value;
  } Settings[] = {
    { 0x1552, 0, 0x4f },
    { 0x1553, 0, 0xf  },
    { 0x1554, 0, 0x4f },
    { 0x1555, 0, 0xf  },
    { 0x1556, 0, 0x4f },
    { 0x1557, 0, 0xf  },
    { 0x155a, 0, 0    },
    { 0x155b, 0, 1    },
    { 0x155c, 0, 1    },
    { 0x155d, 0, 250  },
    { 0x155e, 0, 0    },
    { 0x155f, 0, 0    },
    { 0x1585, 0, 1    },
    { 0x15aa, 0, 1    },
    { 0x15ab, 0, 0    },
    { 0x15a3, 0, 15   },
    { 0x15a8, 0, 0x64 },
    { 0x1564, 0, 0x64 },
    { 0x008d, 4, 0xb  },
    { 0x008d, 5, 0xb  },
    { 0x00f4, 4, 0x9f },
    { 0x00f4, 5, 0x9f },
    { 0x008e, 4, 15   },
    { 0x008e, 5, 15   },
    { 0x0088, 4, 0xff },
    { 0x0088, 5, 0xff },
    { 0x000f, 0, 0x64 },
    { 0x000f, 1, 0x64 },
    { 0x0092, 4, 0x64 },
    { 0x0092, 5, 0x64 },
    { 0x00cd, 0, 5    },
    { 0x00f2, 4, 0x9f },
    { 0x00f2, 5, 0x9f },
    { 0x00fc, 0, 0xfc } // PHY ECO bypass.
  };

  // Clear device and PHY state retained across a warm reboot.
  MmioWrite32 (mBase + UFS_DEVICE_IO, 0);
  MicroSecondDelay (UFS_PHY_BACKDOOR_US);
  MmioWrite32 (mBase + UFS_MPHY_RESET, 0);
  MicroSecondDelay (UFS_PHY_BACKDOOR_US);
  MmioWrite32 (mBase + UFS_MPHY_POWER, 0);
  MicroSecondDelay (UFS_PHY_BACKDOOR_US);
  MmioWrite32 (mBase + UFS_MPHY_RESET, 3);
  MicroSecondDelay (UFS_PHY_STEP_US);
  MmioWrite32 (mBase + UFS_MPHY_POWER, 0x87f);
  MicroSecondDelay (UFS_PHY_STEP_US);
  MmioWrite32 (mBase + UFS_MPHY_POWER, 0xb7f);
  MicroSecondDelay (UFS_PHY_STEP_US);
  MmioWrite32 (mBase + UFS_MPHY_POWER, 0x87f);
  MicroSecondDelay (UFS_PHY_STEP_US);
  MmioWrite32 (mBase + UFS_DEVICE_IO, BIT8 | BIT0);
  MicroSecondDelay (UFS_PHY_STEP_US);
  EFI_STATUS  Status = Wait32 (mBase + UFS_MPHY_POWER, UFS_MPHY_PLL_LOCK, UFS_MPHY_PLL_LOCK, UFS_PLL_TIMEOUT_US);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (mBase + UFS_MPHY_BACKDOOR, 1);
  MicroSecondDelay (UFS_PHY_BACKDOOR_US);
  MmioWrite32 (mBase + UFS_ATOP + (ANA_HSGEAR_CTRL << 2), 0);
  MmioWrite32 (mBase + UFS_ATOP + (0xc2 << 2), 0);
  MicroSecondDelay (UFS_PHY_BACKDOOR_US);
  MmioWrite32 (mBase + UFS_MPHY_BACKDOOR, 0);
  MicroSecondDelay (UFS_PHY_BACKDOOR_US);
  MicroSecondDelay (UFS_PHY_SETTLE_US);
  for (UINTN I = 0; I < ARRAY_SIZE (Settings); I++) {
    Status = DmeSet (Driver, Settings[I].Attr, Settings[I].Sel, Settings[I].Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
Override (
  EFI_HANDLE         Handle,
  EDKII_UFS_HC_INFO  *Info
  )
{
  if ((Handle != mHandle) || !Info) {
    return EFI_INVALID_PARAMETER;
  }

  // UFS must address high DRAM directly; never fabricate 64-bit capability.
  return (Info->Capabilities & BIT24) ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

STATIC EFI_STATUS
EFIAPI
Callback (
  EFI_HANDLE                            Handle,
  EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE  Phase,
  VOID                                  *Data
  )
{
  EDKII_UFS_HC_DRIVER_INTERFACE  *Driver = Data;

  if ((Handle != mHandle) || !Driver || (Driver->UfsHcProtocol != &mHc) || !Driver->UfsExecUicCommand) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_STATUS  Status;

  switch (Phase) {
    case EdkiiUfsHcPreHce:
      return EFI_SUCCESS; // The standard bus driver owns HCE reset and timeout.
    case EdkiiUfsHcPostHce:
      Status = Phy (Driver);
      if (!EFI_ERROR (Status)) {
        MmioWrite32 (mBase + UFS_HC_AHIT_OFFSET, 0);
      }

      return Status;
    case EdkiiUfsHcPreLinkStartup:
      MmioWrite32 (mBase + 0xc0, (UFS_ACLK_HZ + 500000) / 1000000);
      MmioWrite32 (mBase + 0xc4, 0x800);
      MmioWrite32 (mBase + 0xd8, 0xfffffff0);
      return EFI_SUCCESS;
    case EdkiiUfsHcPostLinkStartup:
      if (!(MmioRead32 (mBase + UFS_HC_STATUS_OFFSET) & BIT0)) {
        return EFI_NO_MEDIA;
      }

      (VOID)MmioRead32 (mBase + 0x38);
      Status = DmeSet (Driver, 0xe8, 0, 0x97);
      if (!EFI_ERROR (Status)) {
        Status = DmeSet (Driver, 0xe8, 0, 0xd7);
      }

      if (!EFI_ERROR (Status)) {
        Status = DmeSet (Driver, 0xe8, 0, 0x17);
      }

      if (!EFI_ERROR (Status)) {
        Status = DmeSet (Driver, 0x2041, 0, 0xffff);
      }

      if (!EFI_ERROR (Status)) {
        Status = HighSpeed (Driver);
      }

      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "K3 UFS high-speed initialization: %r\n", Status));
      }

      return Status;
    default: return EFI_INVALID_PARAMETER;
  }
}

STATIC VOID
ReleaseClocks (
  VOID
  )
{
  if (!mClock) {
    return;
  }

  mClock->Reset (mReset, TRUE);
  if (mAxiEnabled) {
    mClock->Enable (mAxiClock, FALSE);
    mAxiEnabled = FALSE;
  }

  if (mRefEnabled) {
    mClock->Enable (mRefClock, FALSE);
    mRefEnabled = FALSE;
  }
}

STATIC EFI_STATUS
Resources (
  CONST VOID  *Fdt,
  INT32       Node
  )
{
  EFI_STATUS  Status = K3FdtRegister (Fdt, Node, 0, 0x2000, &mBase, &mSize);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((mBase != 0xc0e00000) || (mSize != 0x40000)) {
    return EFI_UNSUPPORTED;
  }

  Status = gBS->LocateProtocol (&gSpacemitClockProtocolGuid, NULL, (VOID **)&mClock);
  if (EFI_ERROR (Status) || !mClock || (mClock->Revision != 1)) {
    return EFI_NOT_READY;
  }

  Status = mClock->Resolve (Fdt, Node, "axi", FALSE, &mAxiClock);
  if (!EFI_ERROR (Status)) {
    Status = mClock->Resolve (Fdt, Node, "ref", FALSE, &mRefClock);
  }

  if (!EFI_ERROR (Status)) {
    Status = mClock->Resolve (Fdt, Node, NULL, TRUE, &mReset);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3MapMmio (mBase, mSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  UINT64  Actual;

  UINT64  Ref;

  if (mClock->IsEnabled (mAxiClock) && !mClock->IsResetAsserted (mReset)) {
    Status = mClock->GetRate (mAxiClock, &Actual);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    MmioWrite32 (mBase + UFS_HC_ENABLE_OFFSET, 0);
    Status = Wait32 (mBase + UFS_HC_ENABLE_OFFSET, BIT0, 0, 100000);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = mClock->Reset (mReset, TRUE);
  if (!EFI_ERROR (Status)) {
    Status = mClock->Enable (mAxiClock, FALSE);
  }

  if (!EFI_ERROR (Status)) {
    Status = mClock->SetRate (mAxiClock, UFS_ACLK_HZ, &Actual);
  }

  if (EFI_ERROR (Status) || (Actual != UFS_ACLK_HZ)) {
    return EFI_ERROR (Status) ? Status : EFI_UNSUPPORTED;
  }

  Status = mClock->Enable (mRefClock, TRUE);
  if (!EFI_ERROR (Status)) {
    mRefEnabled = TRUE;
    Status      = mClock->Enable (mAxiClock, TRUE);
  }

  if (!EFI_ERROR (Status)) {
    mAxiEnabled = TRUE;
    Status      = mClock->Reset (mReset, FALSE);
  }

  if (!EFI_ERROR (Status)) {
    Status = mClock->GetRate (mRefClock, &Ref);
  }

  if (!EFI_ERROR (Status) && (Ref != 19200000)) {
    Status = EFI_UNSUPPORTED;
  }

  if (EFI_ERROR (Status)) {
    ReleaseClocks ();
    return Status;
  }

  UINT32  Cap = MmioRead32 (mBase);

  if (!Cap || (Cap == MAX_UINT32) || !(Cap & BIT24)) {
    ReleaseClocks ();
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

#pragma pack(1)
STATIC struct {
  VENDOR_DEVICE_PATH          Vendor;
  UINT64                      Base;
  EFI_DEVICE_PATH_PROTOCOL    End;
} mPath = {
  {
    { HARDWARE_DEVICE_PATH,                          HW_VENDOR_DP,
        { sizeof (VENDOR_DEVICE_PATH) + sizeof (UINT64),
        0 }
    },
    { 0xc8ff0280,                                    0x40bc,                        0x49c1,
        { 0xb6,                                          0x84,
        0xef, 0x43,
        0x2c, 0xc3, 0x22,
        0x8c }
    }
  },
  0xc0e00000,
  { END_DEVICE_PATH_TYPE,                          END_ENTIRE_DEVICE_PATH_SUBTYPE,
        { sizeof (EFI_DEVICE_PATH_PROTOCOL),
        0 }
  }
};
#pragma pack()

EFI_STATUS
EFIAPI
K3UfsEntry (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  CONST EFI_GUID          Guid = RISCV_SEC_HANDOFF_HOB_GUID;
  EFI_HOB_GUID_TYPE       *Hob = GetFirstGuidHob (&Guid);
  RISCV_SEC_HANDOFF_DATA  Handoff;

  if (!Hob || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (Handoff))) {
    return EFI_NOT_FOUND;
  }

  CopyMem (&Handoff, GET_GUID_HOB_DATA (Hob), sizeof (Handoff));
  VOID  *Fdt = Handoff.FdtPointer;

  if (!Fdt || FdtCheckHeader (Fdt) || (FdtTotalSize (Fdt) > SIZE_1MB)) {
    return EFI_COMPROMISED_DATA;
  }

  INT32  Node = FdtNodeOffsetByCompatible (Fdt, -1, "spacemit,k3-ufshc");

  if (!K3FdtEnabled (Fdt, Node)) {
    return EFI_NOT_FOUND;
  }

  EFI_STATUS  Status = Resources (Fdt, Node);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "K3 UFS resource setup: %r\n", Status));
    return Status;
  }

  mHc = (EDKII_UFS_HOST_CONTROLLER_PROTOCOL) {
    Bar, Allocate, Free, Map, Unmap, Flush, Read, Write
  };
  STATIC EDKII_UFS_HC_PLATFORM_PROTOCOL  Platform = {
    EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION, Override, Callback, EdkiiUfsCardRefClkFreq19p2Mhz, FALSE, FALSE
  };

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mPath,
                  &gEdkiiUfsHostControllerProtocolGuid,
                  &mHc,
                  &gEdkiiUfsHcPlatformProtocolGuid,
                  &Platform,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    ReleaseClocks ();
  }

  return Status;
}
