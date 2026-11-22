/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/CpuMmioAddressLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/DwI2cReadLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/FdtI2cReadLib.h>

EFI_STATUS
EFIAPI
FdtI2cReadRegisters (
  IN  CONST VOID  *Fdt,
  IN  INT32       Controller,
  IN  UINT8       Address,
  IN  UINT8       Register,
  OUT UINT8       *Data,
  IN  UINTN       Count
  )
{
  if (Fdt == NULL || Controller < 0 || Address > 0x7f || Data == NULL || Count == 0) {
    return EFI_INVALID_PARAMETER;
  }
  if (!FdtNodeEnabled (Fdt, Controller) ||
      FdtStringListSearch (Fdt, Controller, "compatible", "snps,designware-i2c") < 0) {
    return EFI_UNSUPPORTED;
  }
  UINTN Base, Gate;
  EFI_STATUS Status = FdtGetMmio (Fdt, Controller, 0, EFI_PAGE_SIZE, &Base, NULL);
  if (!EFI_ERROR (Status)) { Status = FdtMapMmio (Base, EFI_PAGE_SIZE); }
  if (EFI_ERROR (Status)) { return Status; }
  INT32 Clock = FdtNodeOffsetByCompatible (Fdt, -1, "sophgo,sg2042-clkgen");
  Status = FdtGetMmio (Fdt, Clock, 0, EFI_PAGE_SIZE, &Gate, NULL);
  if (!EFI_ERROR (Status)) { Status = FdtMapMmio (Gate, EFI_PAGE_SIZE); }
  if (EFI_ERROR (Status)) { return Status; }

  Gate = CpuMmioAddress (Gate);
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  BOOLEAN ClockOn = (MmioRead32 (Gate) & BIT26) != 0;
  if (!ClockOn) { MmioOr32 (Gate, BIT26); }
  Status = DwI2cReadRegisters (CpuMmioAddress (Base), Address, Register, Data, Count);
  if (!ClockOn) { MmioAnd32 (Gate, ~((UINT32)BIT26)); }
  gBS->RestoreTPL (OldTpl);
  return Status;
}
