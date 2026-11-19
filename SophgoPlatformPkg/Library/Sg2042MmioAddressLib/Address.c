/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/CpuMmioAddressLib.h>
#include <Library/BaseLib.h>
#include <Register/RiscV64/RiscVEncoding.h>

UINTN
EFIAPI
CpuMmioAddress (
  IN UINTN  Address
  )
{
  // Socket 0 places the four PCIe apertures and the peripheral registers
  // above the lower Sv39 canonical range. The PTE retains the physical PPN.
  if ((((Address >= 0x4000000000ULL) && (Address < 0x5000000000ULL)) ||
       ((Address >= 0x7000000000ULL) && (Address < 0x7100000000ULL))) &&
      ((RiscVGetSupervisorAddressTranslationRegister () >> SATP64_MODE_SHIFT) == SATP_MODE_SV39))
  {
    return Address | ~(BIT39 - 1);
  }

  return Address;
}
