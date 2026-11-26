/** @file
  Two-phase link training for DesignWare PCIe root ports.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef DWC_PCIE_TRAINING_LIB_H_
#define DWC_PCIE_TRAINING_LIB_H_

#include <Uefi.h>

typedef struct {
  CONST CHAR8    *Name;
  UINTN          Dbi;
  UINT8          MaxSpeed;
  UINT8          MaxWidth;
  VOID           *Context;

  // Return TRUE only in L0, with the link up and DBI accessible.
  BOOLEAN (EFIAPI *LinkReady)(VOID *Context);
  VOID (EFIAPI *EnableLtssm)(VOID *Context);

  // Read an aligned dword from the immediate link partner, bus 1:0.0.
  EFI_STATUS (EFIAPI *ReadPartner)(VOID *Context, UINT16 Register, UINT32 *Value);
  // Return EFI_NO_MEDIA if the initial poll never sees a ready link.
  BOOLEAN AllowEmpty;
  // Accept a stable lower speed or width after the directed training timeout.
  BOOLEAN AllowReducedLink;
} DWC_PCIE_TRAINING;

/**
  Establish a Gen1 link, then request the highest common speed up to Gen3.

  The caller supplies initialized clocks, PHY, lanes and bus numbers, with
  LTSSM disabled and DBI accessible. MaxSpeed and MaxWidth are the platform
  limits, including board wiring. The immediate partner may be a switch.

  On success, LinkStatus contains the stable negotiated speed and width.
  Both must match the selected limits unless AllowReducedLink is set.
  If AllowEmpty is set and LinkReady stays false during the initial poll,
  return EFI_NO_MEDIA with LTSSM enabled. For other errors, reset the
  controller and endpoint before reuse.
**/
EFI_STATUS
EFIAPI
DwcPcieTrainLink (
  IN CONST DWC_PCIE_TRAINING  *Training,
  OUT UINT16                *LinkStatus
  );

#endif
