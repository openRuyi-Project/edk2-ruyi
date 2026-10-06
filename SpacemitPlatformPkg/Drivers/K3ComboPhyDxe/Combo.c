/** SPDX-License-Identifier: BSD-2-Clause-Patent */

/* PHY register sequence adapted from the pinned BSD K3 PCIe reference;
 * register offsets and shared topology cross-checked against the K3 bindings.
 * Copyright (c) 2017, Linaro Ltd. All rights reserved.
 * Copyright (c) 2019 Marvell International Ltd. All rights reserved.
 * Copyright (c) 2025 Spacemit Ltd. All rights reserved.
 */
#include <PiDxe.h>
#include <Guid/RiscVSecHobData.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/K3HardwareLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/ComboPhy.h>

STATIC UINTN    mBase, mSpare;
STATIC UINT32   mMatrix;
STATIC INT32    mNode;
STATIC BOOLEAN  mInitialized[6];
STATIC INT32    mClaims[6];

STATIC EFI_STATUS
Wait (
  UINTN   Register,
  UINT32  Mask,
  UINTN   TimeoutUs
  )
{
  for (UINTN Elapsed = 0; Elapsed < TimeoutUs; Elapsed += 500) {
    if ((MmioRead32 (Register) & Mask) == Mask) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (500);
  }

  return EFI_TIMEOUT;
}

STATIC BOOLEAN
PcieLane (
  UINT32  Index
  )
{
  if ((Index < 2) || (Index == 5)) {
    return TRUE;
  }

  if (Index == 2) {
    return (mMatrix & 0x14) != 0x14;
  }

  if (Index == 3) {
    return (mMatrix & 0x12) != 0x12;
  }

  return (mMatrix & 0x11) != 0x11;
}

STATIC EFI_STATUS
CheckUsbOwners (
  UINTN  Apmu
  )
{
  static CONST UINTN  Bases[] = { 0x81400000, 0x81700000, 0x81a00000 };
  UINT32              Clocks  = MmioRead32 (Apmu + 0x5c);

  for (UINTN I = 0; I < ARRAY_SIZE (Bases); I++) {
    UINT32  Mask = 0xfU << (8 + I * 4);
    if ((Clocks & Mask) != Mask) {
      continue;
    }

    EFI_STATUS  Status = K3MapMmio (Bases[I], 0x10000);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (((MmioRead32 (Bases[I] + 0xc120) & 0xffff0000) != 0x55330000) ||
        (MmioRead32 (Bases[I] + 0xc704) & BIT31))
    {
      return EFI_ACCESS_DENIED;
    }

    // A forced USB2 clock does not consume a combo lane. Keep independent
    // HS controllers usable while this provider prepares PCIe/USB3 lanes.
    if (MmioRead32 (Bases[I] + 0xc11c) & BIT26) {
      continue;
    }

    UINTN  Length = MmioRead32 (Bases[I]) & 0xff;
    if ((Length < 0x20) || (Length > 0x100) || Length % 4 ||
        (MmioRead32 (Bases[I] + Length) & BIT0) ||
        !(MmioRead32 (Bases[I] + Length + 4) & BIT0))
    {
      return EFI_ACCESS_DENIED;
    }
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
InitializePcieGroup (
  UINT32  Index
  )
{
  UINTN   Base      = mBase + Index * 0x100000;
  UINT32  LaneCount = Index < 2 ? 2 : 1;

  if (mInitialized[Index]) {
    return EFI_SUCCESS;
  }

  MmioAndThenOr32 (Base + 0x58, ~0xf000U, 0x2000);
  MmioAnd32 (Base + 0x5c, ~BIT21);
  for (UINT32 Lane = 0; Lane < LaneCount; Lane++) {
    MmioAnd32 (Base + Lane * 0x400 + 0x50, ~3U);
  }

  MmioOr32 (Base + 0x5c, BIT20);
  // The PHY requires a complete refclock-driver register write to renegotiate.
  MmioWrite32 (Base + 0x50, 0x6505);
  MmioAnd32 (Base + 0x58, ~0x0f000000U);
  for (UINT32 Lane = 0; Lane < LaneCount; Lane++) {
    UINTN  B = Base + Lane * 0x400;
    MmioOr32 (B + 0x40, BIT13);
    MmioAnd32 (B + 4, ~BIT6);
    MmioOr32 (B + 0xc, BIT2);
    MmioWrite32 (B + 0x60, 0xdf987810);
    MmioAndThenOr32 (B + 0x64, 0xff000000, 0x002888b4);
    MmioOr32 (B + 8, BIT11);
  }

  EFI_STATUS  Status = Wait (Base + 8, BIT0, 500000);

  if (!EFI_ERROR (Status)) {
    mInitialized[Index] = TRUE;
  }

  return Status;
}

STATIC EFI_STATUS EFIAPI
Pcie (
  CONST VOID  *Fdt,
  INT32       Consumer,
  UINT32      Lanes
  )
{
  INT32         Length;
  CONST UINT32  *Phys = FdtGetProp (Fdt, Consumer, "phys", &Length);
  UINT32        Mask = 0, Total = 0;

  if (!Phys || (Length <= 0) || Length % 12) {
    return EFI_COMPROMISED_DATA;
  }

  for (INT32 I = 0; I < Length / 12; I++) {
    UINT32  Index = Fdt32ToCpu (Phys[I * 3 + 1]);
    if ((FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Phys[I * 3])) != mNode) ||
        (Fdt32ToCpu (Phys[I * 3 + 2]) != 2) || (Index >= 6) || !PcieLane (Index) ||
        (Mask & (1U << Index)))
    {
      return EFI_UNSUPPORTED;
    }

    Mask  |= 1U << Index;
    Total += Index < 2 ? 2 : 1;
  }

  if (Total != Lanes) {
    return EFI_COMPROMISED_DATA;
  }

  UINTN       Dbi;
  INT32       Parent        = FdtParentOffset (Fdt, Consumer);
  INT32       RegisterIndex = FdtStringListSearch (Fdt, Parent, "reg-names", "dbi");
  EFI_STATUS  Status        = K3FdtRegister (Fdt, Parent, (UINTN)RegisterIndex, 4096, &Dbi, NULL);

  if (EFI_ERROR (Status) || (Dbi < 0x80000000) || (Dbi > 0x81000000) ||
      (Dbi - 0x80000000) % 0x400000)
  {
    return EFI_COMPROMISED_DATA;
  }

  UINT32  Id = (UINT32)((Dbi - 0x80000000) / 0x400000);

  if (Id == 0) {
    if (Lanes == 8) {
      if ((mMatrix & BIT4) || (Mask != 0x3f)) {
        return EFI_UNSUPPORTED;
      }
    } else if (Lanes == 4) {
      if (((mMatrix & (BIT4 | BIT3)) != BIT4) || (Mask != 3)) {
        return EFI_UNSUPPORTED;
      }
    } else if (Lanes == 2) {
      if (((mMatrix & (BIT4 | BIT3)) != (BIT4 | BIT3)) || (Mask != 1)) {
        return EFI_UNSUPPORTED;
      }
    } else {
      return EFI_UNSUPPORTED;
    }
  } else {
    if (!(mMatrix & BIT4)) {
      return EFI_UNSUPPORTED;
    }

    if (((Id == 1) && (((mMatrix & BIT3) == 0) || (Mask != 2))) ||
        ((Id == 2) && (Mask != 4) && (Mask != 8) && (Mask != 12)) ||
        ((Id == 3) && (Mask != 16)) || ((Id == 4) && (Mask != 32)))
    {
      return EFI_UNSUPPORTED;
    }
  }

  for (UINT32 I = 0; I < 6; I++) {
    if ((Mask & (1U << I)) && mClaims[I] && (mClaims[I] != Consumer)) {
      return EFI_ACCESS_DENIED;
    }
  }

  for (UINT32 I = 0; I < 6; I++) {
    if (Mask & (1U << I)) {
      mClaims[I] = Consumer;
    }
  }

  for (UINT32 I = 0; I < 6; I++) {
    if (Mask & (1U << I)) {
      EFI_STATUS  Status = InitializePcieGroup (I);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
Usb3 (
  CONST VOID  *Fdt,
  INT32       Consumer
  )
{
  INT32         Index = FdtStringListSearch (Fdt, Consumer, "phy-names", "usb3-phy"), Length;
  CONST UINT32  *Phys = FdtGetProp (Fdt, Consumer, "phys", &Length);

  if ((Index < 0) || !Phys || (Length != 16) || (Index != 1) ||
      (FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Phys[1])) != mNode) ||
      (Fdt32ToCpu (Phys[3]) != 4))
  {
    return EFI_COMPROMISED_DATA;
  }

  UINT32  Group = Fdt32ToCpu (Phys[2]);

  if ((Group < 2) || (Group > 4) || PcieLane (Group)) {
    return EFI_UNSUPPORTED;
  }

  UINTN       Controller;
  EFI_STATUS  Status = K3FdtRegister (Fdt, Consumer, 0, 0x10000, &Controller, NULL);

  if (EFI_ERROR (Status) || (Controller != 0x81400000 + (Group - 2) * 0x300000)) {
    return EFI_COMPROMISED_DATA;
  }

  if (mClaims[Group] && (mClaims[Group] != Consumer)) {
    return EFI_ACCESS_DENIED;
  }

  if (mInitialized[Group]) {
    return EFI_SUCCESS;
  }

  mClaims[Group] = Consumer;
  UINTN  Base = mBase + Group * 0x100000;

  MmioAnd32 (Base + 4, ~BIT6);
  MmioAnd32 (Base + 0x54, ~BIT25);
  // 24MHz refclk, 5000ppm down-spread SSC, matching the K3 USB recipe.
  MmioWrite32 (Base + 0x58, 0x3a002000);
  MmioAnd32 (Base + 0x5c, ~BIT21);
  MmioAndThenOr32 (Base + 0xc, ~(BIT8 | BIT9), BIT8 | BIT9);
  MmioOr32 (Base + 0x140, BIT1 | BIT4);
  MmioAndThenOr32 (Base + 0xb4, ~(3U << 22), BIT24 | (3U << 22));
  MmioWrite32 (Base + 0x60, 0xdf987810);
  MmioAndThenOr32 (Base + 0x64, 0xff000000, 0x002888b4);
  MmioWrite32 (Base + 8, BIT11 | BIT23 | (2U << 7) | BIT3 | BIT4 | BIT5 | BIT6);
  Status = Wait (Base + 8, BIT0, 500000);
  if (!EFI_ERROR (Status)) {
    mInitialized[Group] = TRUE;
  }

  return Status;
}

EFI_STATUS EFIAPI
K3ComboEntry (
  EFI_HANDLE        Image,
  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  CONST EFI_GUID          Guid = RISCV_SEC_HANDOFF_HOB_GUID;
  EFI_HOB_GUID_TYPE       *Hob = GetFirstGuidHob (&Guid);
  RISCV_SEC_HANDOFF_DATA  Handoff;
  VOID                    *Fdt;
  INT32                   Length, Provider;
  UINTN                   Bytes, Apmu;
  EFI_STATUS              Status;

  if (!Hob || (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (Handoff))) {
    return EFI_NOT_FOUND;
  }

  CopyMem (&Handoff, GET_GUID_HOB_DATA (Hob), sizeof (Handoff));
  Fdt = Handoff.FdtPointer;
  if (!Fdt || FdtCheckHeader (Fdt) || (FdtTotalSize (Fdt) > SIZE_1MB)) {
    return EFI_COMPROMISED_DATA;
  }

  mNode = FdtNodeOffsetByCompatible (Fdt, -1, "spacemit,k3-combo-phy");
  if (!K3FdtEnabled (Fdt, mNode)) {
    return EFI_NOT_FOUND;
  }

  Status = K3FdtRegister (Fdt, mNode, 0, 0x600000, &mBase, &Bytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3MapMmio (mBase, Bytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CONST UINT32  *Cells = FdtGetProp (Fdt, mNode, "spacemit,apb-spare", &Length);

  if (!Cells || (Length != 4)) {
    return EFI_COMPROMISED_DATA;
  }

  Provider = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Cells));
  if (!K3FdtEnabled (Fdt, Provider) || (FdtStringListSearch (Fdt, Provider, "compatible", "spacemit,k3-pll") < 0)) {
    return EFI_COMPROMISED_DATA;
  }

  Status = K3FdtRegister (Fdt, Provider, 0, 0x180, &mSpare, &Bytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3MapMmio (mSpare, Bytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Cells = FdtGetProp (Fdt, mNode, "spacemit,apmu", &Length);
  if (!Cells || (Length != 8) || (Fdt32ToCpu (Cells[1]) & ~0x1fU)) {
    return EFI_COMPROMISED_DATA;
  }

  mMatrix  = Fdt32ToCpu (Cells[1]);
  Provider = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Cells[0]));
  if (!K3FdtEnabled (
         Fdt,
         Provider
         ) || (FdtStringListSearch (Fdt, Provider, "compatible", "spacemit,k3-syscon-apmu") < 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  Status = K3FdtRegister (Fdt, Provider, 0, 0x200, &Apmu, &Bytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = K3MapMmio (Apmu, Bytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // This cold-init owner refuses to change a matrix with active PCIe LTSSMs.
  static CONST UINT32  Controls[] = { 0x1f0, 0x1f8, 0x1c8, 0x1b8, 0x1e8 };

  for (UINTN I = 0; I < ARRAY_SIZE (Controls); I++) {
    if (MmioRead32 (Apmu + Controls[I]) & BIT6) {
      return EFI_ACCESS_DENIED;
    }
  }

  Status = CheckUsbOwners (Apmu);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioAndThenOr32 (Apmu + 0x1d8, ~0x1fU, mMatrix);
  if (!(MmioRead32 (mSpare + 0x17c) & BIT8)) {
    MmioOr32 (mSpare + 0x178, BIT17);
    Status = Wait (mSpare + 0x17c, BIT8, 2000000);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "K3 combo PHY calibration did not complete\n"));
      return Status;
    }
  }

  static SPACEMIT_COMBO_PHY_PROTOCOL  Protocol;

  Protocol = (SPACEMIT_COMBO_PHY_PROTOCOL) {
    SPACEMIT_COMBO_PHY_REVISION, mNode, Pcie, Usb3
  };
  EFI_HANDLE  Handle = NULL;

  return gBS->InstallProtocolInterface (
                &Handle,
                &gSpacemitComboPhyProtocolGuid,
                EFI_NATIVE_INTERFACE,
                &Protocol
                );
}
