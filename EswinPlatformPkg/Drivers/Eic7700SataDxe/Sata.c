/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * PHY setup follows Linux drivers/phy/eswin/phy-eic7700-sata.c.
 * Copyright 2026, Beijing ESWIN Computing Technology Co., Ltd.
 * Yulin Lu <luyulin@eswincomputing.com>
 */
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtBoardInfoLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

// EIC7700 clock/reset binding IDs and their SYSCRG registers.
#define CLK_HSP_CFG       171
#define CLK_HSP_ACLK      186
#define CLK_SATA_REF      187
#define HSP_CLK_SATA      17
#define RESET_SATA_ARST   96
#define HSP_RESET_PORT    0
#define HSP_RESET_PHY     1
#define SYSCRG_HSP_RESET  0x11c
#define SYSCRG_HSP_ACLK   0x148
#define SYSCRG_HSP_CFG    0x14c
#define SYSCRG_SATA_REF   0x154
#define SATA_ARST_N       BIT27

#define HSP_SATA_BASE     0x300
#define HSP_SATA_CLOCK    BIT28
#define HSP_SATA_RESET    0x340
#define HSP_SATA_RESETS   (BIT0 | BIT1)
#define SATA_AXI_LP_CTRL  0x08
#define SATA_MPLL_CTRL    0x20
#define SATA_PHY_STATUS  0x24
#define SATA_PHY_CTRL0   0x28
#define SATA_PHY_CTRL1   0x2c
#define SATA_REF_CTRL    0x34
#define SATA_REF_CTRL1   0x38
#define SATA_LOS_IDEN    0x3c
#define SATA_CSYSREQ     (BIT0 | BIT16)
#define SATA_REF_REPEAT  BIT0
#define SATA_REF_PAD     BIT20
#define SATA_PHY_READY   BIT0

#define AHCI_CAP         0x00
#define AHCI_PI          0x0c
#define AHCI_PORT0_CMD   0x118
#define AHCI_CMD_ST      BIT0
#define AHCI_CMD_FRE     BIT4
#define AHCI_CMD_FR      BIT14
#define AHCI_CMD_CR      BIT15

typedef struct {
  UINTN Base, Bytes, Clock, Reset, Hsp, Phy;
  UINT32 Amplitude, Preemphasis;
  EFI_EVENT ExitEvent;
} SATA_CONTEXT;

STATIC EFI_STATUS
Wait (UINTN Address, UINT32 Mask, UINT32 Expected, UINTN TimeoutUs)
{
  UINT32 Value = 0;

  for (UINTN Elapsed = 0; Elapsed < TimeoutUs; Elapsed += 10) {
    Value = MmioRead32 (Address);
    if ((Value & Mask) == Expected) { return EFI_SUCCESS; }
    MicroSecondDelay (10);
  }
  DEBUG ((DEBUG_ERROR, "EIC7700 SATA timeout at %Lx: %08x\n", (UINT64)Address, Value));
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
Provider (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Property,
          CONST CHAR8 *Names, CONST CHAR8 *Name, CONST CHAR8 *Compatible,
          UINT32 Id, UINTN Minimum, UINTN *Base, INT32 *ProviderNode OPTIONAL)
{
  INT32 Index, Length, Parent;
  CONST UINT32 *Data;

  Index = FdtStringListSearch (Fdt, Node, Names, Name);
  Data = FdtGetProp (Fdt, Node, Property, &Length);
  if (Index < 0 || !Data || Length <= 0 || Length % (2 * sizeof (*Data)) ||
      (UINTN)Index >= (UINTN)Length / (2 * sizeof (*Data))) {
    return EFI_COMPROMISED_DATA;
  }
  Data += 2 * Index;
  Parent = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Data[0]));
  if (!FdtNodeEnabled (Fdt, Parent) || Fdt32ToCpu (Data[1]) != Id ||
      FdtStringListSearch (Fdt, Parent, "compatible", Compatible) < 0) {
    return EFI_UNSUPPORTED;
  }
  Data = FdtGetProp (Fdt, Parent, AsciiStrCmp (Property, "clocks") == 0 ?
                     "#clock-cells" : "#reset-cells", &Length);
  if (!Data || Length != sizeof (*Data) || Fdt32ToCpu (*Data) != 1) {
    return EFI_UNSUPPORTED;
  }
  if (ProviderNode) { *ProviderNode = Parent; }
  return FdtGetMmio (Fdt, Parent, 0, Minimum, Base, NULL);
}

STATIC EFI_STATUS
Tuning (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name,
        CONST UINT32 Default[3], UINT32 Limit, UINT32 *Value)
{
  INT32 Length;
  CONST UINT32 *Data = FdtGetProp (Fdt, Node, Name, &Length);

  if (Data && Length != 3 * sizeof (*Data)) { return EFI_COMPROMISED_DATA; }
  *Value = 0;
  for (UINTN I = 0; I < 3; I++) {
    UINT32 V = Data ? Fdt32ToCpu (Data[I]) : Default[I];
    if (V > Limit) { return EFI_UNSUPPORTED; }
    *Value |= V << (8 * I);
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Parse (CONST VOID *Fdt, INT32 Node, SATA_CONTEXT *S)
{
  STATIC CONST UINT32 Amplitude[] = {0x42, 0x46, 0x73};
  STATIC CONST UINT32 Preemphasis[] = {0x05, 0x05, 0x23};
  CONST UINT32 *Data;
  INT32 Length, PhyNode, HspNode;
  UINTN Base;
  EFI_STATUS Status;

  Status = FdtGetMmio (Fdt, Node, 0, AHCI_PORT0_CMD + 4, &S->Base, &S->Bytes);
  if (EFI_ERROR (Status)) { return Status; }
  Data = FdtGetProp (Fdt, Node, "ports-implemented", &Length);
  if (Data && (Length != sizeof (*Data) || Fdt32ToCpu (*Data) != BIT0)) {
    return EFI_UNSUPPORTED;
  }
  Data = FdtGetProp (Fdt, Node, "phys", &Length);
  if (!Data || Length != sizeof (*Data)) { return EFI_COMPROMISED_DATA; }
  PhyNode = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Data));
  if (!FdtNodeEnabled (Fdt, PhyNode) ||
      FdtStringListSearch (Fdt, PhyNode, "compatible", "eswin,eic7700-sata-phy") < 0) {
    return EFI_UNSUPPORTED;
  }
  Status = FdtGetMmio (Fdt, PhyNode, 0, SATA_LOS_IDEN + 4, &S->Phy, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Provider (Fdt, Node, "clocks", "clock-names", "pclk",
                     "eswin,eic7700-clock", CLK_HSP_CFG, SYSCRG_SATA_REF + 4,
                     &S->Clock, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Provider (Fdt, Node, "clocks", "clock-names", "aclk",
                     "eswin,eic7700-clock", CLK_HSP_ACLK, SYSCRG_SATA_REF + 4, &Base, NULL);
  if (EFI_ERROR (Status) || Base != S->Clock) { return EFI_UNSUPPORTED; }
  Status = Provider (Fdt, Node, "resets", "reset-names", "arst",
                     "eswin,eic7700-reset", RESET_SATA_ARST, SYSCRG_HSP_RESET + 4,
                     &S->Reset, NULL);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Provider (Fdt, PhyNode, "clocks", "clock-names", "phy",
                     "eswin,eic7700-hspcrg", HSP_CLK_SATA, HSP_SATA_RESET + 4,
                     &S->Hsp, &HspNode);
  if (EFI_ERROR (Status)) { return Status; }
  if (S->Phy != S->Hsp + HSP_SATA_BASE) { return EFI_UNSUPPORTED; }
  Status = Provider (Fdt, HspNode, "clocks", "clock-names", "sata",
                     "eswin,eic7700-clock", CLK_SATA_REF, SYSCRG_SATA_REF + 4, &Base, NULL);
  if (EFI_ERROR (Status) || Base != S->Clock) { return EFI_UNSUPPORTED; }
  Status = Provider (Fdt, PhyNode, "resets", "reset-names", "port",
                     "eswin,eic7700-hspcrg", HSP_RESET_PORT, HSP_SATA_RESET + 4, &Base, NULL);
  if (EFI_ERROR (Status) || Base != S->Hsp) { return EFI_UNSUPPORTED; }
  Status = Provider (Fdt, PhyNode, "resets", "reset-names", "phy",
                     "eswin,eic7700-hspcrg", HSP_RESET_PHY, HSP_SATA_RESET + 4, &Base, NULL);
  if (EFI_ERROR (Status) || Base != S->Hsp) { return EFI_UNSUPPORTED; }
  Status = Tuning (Fdt, PhyNode, "eswin,tx-amplitude-tuning", Amplitude, 0x7f, &S->Amplitude);
  if (EFI_ERROR (Status)) { return Status; }
  return Tuning (Fdt, PhyNode, "eswin,tx-preemph-tuning", Preemphasis, 0x3f, &S->Preemphasis);
}

STATIC VOID
HoldReset (SATA_CONTEXT *S)
{
  MmioAnd32 (S->Reset + SYSCRG_HSP_RESET, ~(UINT32)SATA_ARST_N);
  MmioOr32 (S->Hsp + HSP_SATA_RESET, HSP_SATA_RESETS);
}

STATIC VOID EFIAPI
ExitBootServices (EFI_EVENT Event, VOID *Context)
{
  SATA_CONTEXT *S = Context;

  // Stop DMA before the OS can reclaim the firmware's command/FIS buffers.
  MmioAnd32 (S->Base + AHCI_PORT0_CMD, ~(UINT32)AHCI_CMD_ST);
  if (!EFI_ERROR (Wait (S->Base + AHCI_PORT0_CMD, AHCI_CMD_CR, 0, 500000))) {
    MmioAnd32 (S->Base + AHCI_PORT0_CMD, ~(UINT32)AHCI_CMD_FRE);
    if (!EFI_ERROR (Wait (S->Base + AHCI_PORT0_CMD, AHCI_CMD_FR, 0, 500000))) {
      return;
    }
  }
  DEBUG ((DEBUG_ERROR, "EIC7700 SATA: forcing reset to stop DMA\n"));
  HoldReset (S);
}

STATIC EFI_STATUS
Initialize (SATA_CONTEXT *S)
{
  EFI_STATUS Status;

  Status = FdtMapMmio (S->Base, S->Bytes);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (S->Clock, SYSCRG_SATA_REF + 4);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (S->Reset, SYSCRG_HSP_RESET + 4);
  if (EFI_ERROR (Status)) { return Status; }
  Status = FdtMapMmio (S->Hsp, HSP_SATA_RESET + 4);
  if (EFI_ERROR (Status)) { return Status; }

  MmioOr32 (S->Clock + SYSCRG_HSP_CFG, BIT31);
  MmioOr32 (S->Clock + SYSCRG_HSP_ACLK, BIT31);
  MmioOr32 (S->Clock + SYSCRG_SATA_REF, BIT31);
  HoldReset (S);
  MmioOr32 (S->Reset + SYSCRG_HSP_RESET, SATA_ARST_N);
  MmioOr32 (S->Hsp + HSP_SATA_BASE, HSP_SATA_CLOCK);
  MmioWrite32 (S->Phy + SATA_REF_CTRL1, BIT0);
  MmioWrite32 (S->Phy + SATA_PHY_CTRL0, S->Amplitude);
  MmioWrite32 (S->Phy + SATA_PHY_CTRL1, S->Preemphasis);
  MmioWrite32 (S->Phy + SATA_LOS_IDEN, 0x9 | (0x2 << 16));
  MmioWrite32 (S->Phy + SATA_AXI_LP_CTRL, SATA_CSYSREQ);
  MmioWrite32 (S->Phy + SATA_REF_CTRL, SATA_REF_REPEAT | SATA_REF_PAD);
  MmioWrite32 (S->Phy + SATA_MPLL_CTRL, 0x3c << 16);
  MicroSecondDelay (15);
  MmioAnd32 (S->Hsp + HSP_SATA_RESET, ~(UINT32)HSP_SATA_RESETS);
  Status = Wait (S->Phy + SATA_PHY_STATUS, SATA_PHY_READY, SATA_PHY_READY, 1000);
  if (EFI_ERROR (Status)) { goto Fail; }
  if (MmioRead32 (S->Base + AHCI_CAP) == MAX_UINT32) {
    Status = EFI_DEVICE_ERROR;
    goto Fail;
  }
  // DWC requires a write to the HW-initialized CAP fields before spin-up.
  // Preserve the advertised capabilities, as Linux does when restoring CAP.
  MmioWrite32 (S->Base + AHCI_CAP, MmioRead32 (S->Base + AHCI_CAP));
  // EIC7700 has one SATA port; PI needs firmware initialization.
  MmioWrite32 (S->Base + AHCI_PI, BIT0);
  return EFI_SUCCESS;
Fail:
  HoldReset (S);
  MmioAnd32 (S->Hsp + HSP_SATA_BASE, ~(UINT32)HSP_SATA_CLOCK);
  return Status;
}

EFI_STATUS EFIAPI
Eic7700SataEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  RUYI_BOARD_INFO Info;
  CONST VOID *Fdt;
  INT32 Node = -1;
  EFI_STATUS Status, Result = EFI_NOT_FOUND;

  Status = FdtBoardInfoGet (&Info, &Fdt);
  if (EFI_ERROR (Status)) { return Status; }
  while ((Node = FdtNodeOffsetByCompatible (Fdt, Node, "eswin,eic7700-ahci")) >= 0) {
    SATA_CONTEXT *S;
    EFI_HANDLE Handle = NULL;

    if (!FdtNodeEnabled (Fdt, Node)) { continue; }
    S = AllocateZeroPool (sizeof (*S));
    if (!S) { return Result == EFI_SUCCESS ? Result : EFI_OUT_OF_RESOURCES; }
    Status = Parse (Fdt, Node, S);
    if (!EFI_ERROR (Status)) { Status = Initialize (S); }
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "EIC7700 SATA node %d: %r\n", Node, Status));
      FreePool (S);
      if (Result != EFI_SUCCESS) { Result = Status; }
      continue;
    }
    Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ExitBootServices, S,
                                &gEfiEventExitBootServicesGuid, &S->ExitEvent);
    if (!EFI_ERROR (Status)) {
      Status = RegisterNonDiscoverableMmioDevice (
                 NonDiscoverableDeviceTypeAhci, NonDiscoverableDeviceDmaTypeNonCoherent,
                 NULL, &Handle, 1, S->Base, S->Bytes);
    }
    if (EFI_ERROR (Status)) {
      if (S->ExitEvent) { gBS->CloseEvent (S->ExitEvent); }
      HoldReset (S);
      MmioAnd32 (S->Hsp + HSP_SATA_BASE, ~(UINT32)HSP_SATA_CLOCK);
      FreePool (S);
      DEBUG ((DEBUG_ERROR, "EIC7700 SATA registration: %r\n", Status));
      if (Result != EFI_SUCCESS) { Result = Status; }
      continue;
    }
    DEBUG ((DEBUG_INFO, "EIC7700 SATA AHCI at %Lx: CAP=%08x PI=%08x\n",
            (UINT64)S->Base, MmioRead32 (S->Base + AHCI_CAP), MmioRead32 (S->Base + AHCI_PI)));
    Result = EFI_SUCCESS;
  }
  return Result;
}
