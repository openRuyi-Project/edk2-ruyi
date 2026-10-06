/** SPDX-License-Identifier: BSD-2-Clause-Patent */

/* DesignWare unrolled iATU layout from the pinned BSD reference controller
 * library; K3 link status matches the board's spacemit,k3-pcie binding.
 * Copyright (c) 2024, SpacemiT Co., Ltd. All rights reserved.
 */
#include <Uefi.h>
#include <Library/DesignWarePciLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define ATU_STRIDE    0x200
#define ATU_ENABLE    BIT31
#define ATU_CONFIG0   4
#define ATU_CONFIG1   5
#define LINK_UP_BITS  (BIT1 | BIT12)

EFI_STATUS
SpacemitPcieProgramWindow (
  SPACEMIT_PCIE_PORT  *Port,
  UINT32              Index,
  UINT32              Type,
  UINT64              Cpu,
  UINT64              Pci,
  UINT64              Size
  )
{
  UINTN   Base;
  UINT64  Limit;

  if ((Port == NULL) || (Port->Atu == 0) || (Port->Atu > MAX_UINTN - 0x800) ||
      (Index >= 4) || !Size ||
      (Cpu > MAX_UINT64 - Size + 1) || (Pci > MAX_UINT64 - Size + 1) ||
      (Cpu & 0xfff) || (Pci & 0xfff) || (Size & 0xfff) ||
      ((Type != 0) && (Type != 2) && (Type != ATU_CONFIG0) && (Type != ATU_CONFIG1)))
  {
    return EFI_INVALID_PARAMETER;
  }

  Limit = Cpu + Size - 1;
  // The initial windows have a single upper address. Do not truncate a
  // crossing window or assume a controller revision's upper-limit extension.
  if ((Cpu >> 32) != (Limit >> 32)) {
    return EFI_UNSUPPORTED;
  }

  Base = Port->Atu + Index * ATU_STRIDE;
  MmioWrite32 (Base + 4, 0);
  MmioWrite32 (Base + 8, (UINT32)Cpu);
  MmioWrite32 (Base + 12, (UINT32)(Cpu >> 32));
  MmioWrite32 (Base + 16, (UINT32)Limit);
  MmioWrite32 (Base + 20, (UINT32)Pci);
  MmioWrite32 (Base + 24, (UINT32)(Pci >> 32));
  MmioWrite32 (Base, Type);
  MemoryFence ();
  MmioWrite32 (Base + 4, ATU_ENABLE);
  for (UINTN Retry = 0; Retry < 100; Retry++) {
    if ((MmioRead32 (Base + 4) & ATU_ENABLE) != 0) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  MmioWrite32 (Base + 4, 0);
  return EFI_TIMEOUT;
}

EFI_STATUS
SpacemitPcieConfigAccess (
  SPACEMIT_PCIE_PORT  *Port,
  UINT8               Bus,
  UINT8               Device,
  UINT8               Function,
  UINT16              Register,
  UINTN               Width,
  BOOLEAN             Write,
  UINT32              *Value
  )
{
  EFI_STATUS  Status = EFI_SUCCESS;
  EFI_TPL     Tpl;
  UINTN       Address;

  if ((Port == NULL) || (Value == NULL) || !Port->Ready ||
      (Bus < Port->BusMin) || (Bus > Port->BusMax) || (Device > 31) || (Function > 7) ||
      ((Width != 1) && (Width != 2) && (Width != 4)) ||
      (Register >= 4096) || (Register & (Width - 1)) || (Width > 4096 - Register))
  {
    return EFI_INVALID_PARAMETER;
  }

  if ((Port->Dbi > MAX_UINTN - 4096) || (Port->Config > MAX_UINTN - Port->ConfigSize) ||
      (Port->Link > MAX_UINTN - 8))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (Bus == Port->BusMin) {
    if (Device || Function || !Port->Dbi) {
      return EFI_NOT_FOUND;
    }

    Address = Port->Dbi + Register;
  } else {
    if (!Port->Config || (Port->ConfigSize < 4096) || !Port->Link ||
        ((Bus == Port->BusMin + 1) && (Device != 0)))
    {
      return EFI_NOT_FOUND;
    }

    if ((MmioRead32 (Port->Link + 4) & LINK_UP_BITS) != LINK_UP_BITS) {
      return EFI_NOT_READY;
    }

    Address = Port->Config + Register;
  }

  // Hold serialization across iATU selection and the actual config access.
  Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  if (Bus != Port->BusMin) {
    UINT64  Target = ((UINT32)Bus << 24) | ((UINT32)Device << 19) | ((UINT32)Function << 16);
    Status = SpacemitPcieProgramWindow (
               Port,
               0,
               Bus == Port->BusMin + 1 ? ATU_CONFIG0 : ATU_CONFIG1,
               Port->Config,
               Target,
               Port->ConfigSize
               );
  }

  if (!EFI_ERROR (Status)) {
    // PCI configuration accesses preserve real byte enables, including W1C
    // registers. Do not emulate partial writes with a destructive DWORD RMW.
    if (Write) {
      if (Width == 1) {
        MmioWrite8 (Address, (UINT8)*Value);
      } else if (Width == 2) {
        MmioWrite16 (Address, (UINT16)*Value);
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
