/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/CpuMmioAddressLib.h>
#include <Library/FdtHardwareLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/Sg2042ClockLib.h>

#define SG2042_MPLL_CONTROL   0x28
#define SG2042_FPLL_CONTROL   0x34
#define SG2042_CLKSEL0        0x20
#define SG2042_CPU_MPLL_DIV   0x40
#define SG2042_CPU_FPLL_DIV   0x44
#define SG2042_EMMC_DIV       0x8c
#define SG2042_SD_DIV         0x94

STATIC
EFI_STATUS
ClockRegisters (
  IN  CONST VOID   *Fdt,
  IN  INT32        Node,
  IN  UINTN        LastRegister,
  OUT UINTN        *Base
  )
{
  EFI_STATUS  Status;
  UINTN       Size;

  Status = FdtGetMmio (Fdt, Node, 0, LastRegister + sizeof (UINT32), Base, &Size);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = FdtMapMmio (*Base, Size);
  if (!EFI_ERROR (Status)) {
    *Base = CpuMmioAddress (*Base);
  }

  return Status;
}

UINT64
Sg2042GetClockRate (
  IN CONST VOID    *Fdt,
  IN SG2042_CLOCK  Clock
  )
{
  INT32         PllNode, ClkNode, RefNode, Index, Len;
  CONST UINT32  *Cells;
  UINTN         PllBase, ClkBase, PllOffset, DivOffset;
  UINT32        Reference, Pll, Div, Factor, Denominator;
  BOOLEAN       UseMpll;

  PllNode = FdtNodeOffsetByCompatible (Fdt, -1, "sophgo,sg2042-pll");
  ClkNode = FdtNodeOffsetByCompatible (Fdt, -1, "sophgo,sg2042-clkgen");
  if ((PllNode < 0) || (ClkNode < 0)) {
    return 0;
  }

  // The PLL inputs are fixed clocks with no clock specifier cells.
  Index = FdtStringListSearch (Fdt, PllNode, "clock-names", "cgi_main");
  Cells = FdtGetProp (Fdt, PllNode, "clocks", &Len);
  if ((Index < 0) || (Cells == NULL) || (Len < 0) || (Index >= Len / (INT32)sizeof (*Cells))) {
    return 0;
  }

  RefNode = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Cells[Index]));
  if (RefNode < 0) {
    return 0;
  }

  Cells = FdtGetProp (Fdt, RefNode, "clock-frequency", &Len);
  if ((Cells == NULL) || (Len != sizeof (*Cells))) {
    return 0;
  }

  Reference = Fdt32ToCpu (*Cells);
  if (EFI_ERROR (ClockRegisters (Fdt, PllNode, SG2042_FPLL_CONTROL, &PllBase)) ||
      EFI_ERROR (ClockRegisters (Fdt, ClkNode, SG2042_SD_DIV, &ClkBase))) {
    return 0;
  }

  PllOffset = SG2042_FPLL_CONTROL;
  switch (Clock) {
    case Sg2042CpuClock:
      UseMpll   = (MmioRead32 (ClkBase + SG2042_CLKSEL0) & BIT0) != 0;
      PllOffset = UseMpll ? SG2042_MPLL_CONTROL : SG2042_FPLL_CONTROL;
      DivOffset = UseMpll ? SG2042_CPU_MPLL_DIV : SG2042_CPU_FPLL_DIV;
      Factor    = 1;
      break;
    case Sg2042SdClock:
      DivOffset = SG2042_SD_DIV;
      Factor    = 10;
      break;
    case Sg2042EmmcClock:
      DivOffset = SG2042_EMMC_DIV;
      Factor    = 10;
      break;
    default:
      return 0;
  }

  Pll = MmioRead32 (PllBase + PllOffset);
  Div = MmioRead32 (ClkBase + DivOffset);
  // Bit 3 selects the programmed divider over the hardware reset factor.
  if ((Div & BIT3) != 0) {
    Factor = MAX (1U, (Div >> 16) & 0x1f);
  }

  Denominator = (Pll & 0x3f) * ((Pll >> 8) & 7) * ((Pll >> 12) & 7) * Factor;
  return (Denominator != 0) ? ((UINT64)Reference * ((Pll >> 16) & 0xfff)) / Denominator : 0;
}
