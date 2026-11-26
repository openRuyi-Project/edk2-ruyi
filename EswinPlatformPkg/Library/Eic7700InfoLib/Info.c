/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/Eic7700InfoLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>

// EIC7700 SYSCRG register offsets, from the SoC TRM.
#define EIC7700_CPU_PLL_CFG0  0x64
#define EIC7700_CPU_PLL_CFG1  0x68
#define EIC7700_CPU_PLL_CFG2  0x6c
#define EIC7700_CPU_CLK_CTRL  0x208

STATIC UINT64
CoreRate (
  CONST VOID  *Fdt
  )
{
  CONST UINT32                     *Data;
  INT32                            Node, Clock, Length;
  UINT64                           Base;
  UINT32                           Reference, Switch, Config0, Config1, Config2, Fraction;
  UINT32                           RefDiv, PostDivA, PostDivB;
  UINT64                           Rate;
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;

  Node = FdtNodeOffsetByCompatible (Fdt, -1, "eswin,eic7700-clock");
  if (Node < 0) {
    return 0;
  }

  Data = FdtGetProp (Fdt, Node, "reg", &Length);
  if (!Data || (Length != 16)) {
    return 0;
  }

  Base = LShiftU64 (Fdt32ToCpu (Data[0]), 32) | Fdt32ToCpu (Data[1]);
  if ((Base & EFI_PAGE_MASK) || Data[2] || (Fdt32ToCpu (Data[3]) < EIC7700_CPU_CLK_CTRL + sizeof (UINT32))) {
    return 0;
  }

  Data = FdtGetProp (Fdt, Node, "clocks", &Length);
  if (!Data || (Length < 4)) {
    return 0;
  }

  Clock = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (*Data));
  if (Clock < 0) {
    return 0;
  }

  Data = FdtGetProp (Fdt, Clock, "clock-frequency", &Length);
  if (!Data || (Length != 4) || !*Data) {
    return 0;
  }

  Reference = Fdt32ToCpu (*Data);
  Status    = gDS->GetMemorySpaceDescriptor (Base, &Descriptor);
  if (EFI_ERROR (Status) || (Descriptor.GcdMemoryType == EfiGcdMemoryTypeNonExistent)) {
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Base, EFI_PAGE_SIZE, EFI_MEMORY_UC);
    if (EFI_ERROR (Status)) {
      return 0;
    }

    Status = gDS->SetMemorySpaceAttributes (Base, EFI_PAGE_SIZE, EFI_MEMORY_UC);
    if (EFI_ERROR (Status)) {
      return 0;
    }
  } else if ((Descriptor.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) ||
             (Descriptor.BaseAddress > Base) || (Descriptor.Length < EFI_PAGE_SIZE) ||
             (Base - Descriptor.BaseAddress > Descriptor.Length - EFI_PAGE_SIZE))
  {
    return 0;
  }

  // The CPU root selects the CPU PLL, a low-power clock, or the crystal.
  Switch = MmioRead32 (Base + EIC7700_CPU_CLK_CTRL) & 3;
  if (Switch == 2) {
    return Reference;
  }

  if (Switch != 0) {
    return 0;
  }

  Config0 = MmioRead32 (Base + EIC7700_CPU_PLL_CFG0);
  Config1 = MmioRead32 (Base + EIC7700_CPU_PLL_CFG1);
  Config2 = MmioRead32 (Base + EIC7700_CPU_PLL_CFG2);
  RefDiv  = BitFieldRead32 (Config0, 12, 17);
  if (!(Config0 & BIT0) || (Config0 & 0xf0) || !(Config1 & BIT1) || (RefDiv == 0)) {
    return 0;
  }

  // The CPU PLL uses the FOUT1 post-dividers in CFG2.
  PostDivA = BitFieldRead32 (Config2, 8, 10);
  PostDivB = BitFieldRead32 (Config2, 12, 14);
  Fraction = (Config0 & BIT9) ? BitFieldRead32 (Config1, 4, 27) : 0;
  Rate     = MultU64x32 (Reference, BitFieldRead32 (Config0, 20, 31));
  Rate    += RShiftU64 (MultU64x32 (Reference, Fraction) + 0xffffff, 24);
  return DivU64x32 (Rate, 4 * RefDiv * (PostDivA + 1) * (PostDivB + 1));
}

EFI_STATUS
Eic7700GetBoardInfo (
  EIC7700_BOARD_INFO  *Info,
  CONST CHAR8         **SocName
  )
{
  CONST VOID  *Fdt;
  EFI_STATUS  Status;

  Status = FdtBoardInfoGet (Info, &Fdt);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (FdtStringListSearch (Fdt, 0, "compatible", "eswin,eic7702") >= 0) {
    *SocName = "ESWIN EIC7702";
  } else if (FdtStringListSearch (Fdt, 0, "compatible", "eswin,eic7700") >= 0) {
    *SocName = "ESWIN EIC7700";
  } else {
    return EFI_UNSUPPORTED;
  }

  Info->CpuRate = CoreRate (Fdt);
  return EFI_SUCCESS;
}
