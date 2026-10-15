/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/K3CpuConfigLib.h>
#include <Library/K3SpiConfigLib.h>

EFI_STATUS
K3CpuConfigGet (
  UINT32  *Mode
  )
{
  return K3SpiConfigGet (K3SettingCpuMode, Mode);
}

EFI_STATUS
K3CpuConfigSet (
  UINT32  Mode
  )
{
  return K3SpiConfigSet (K3SettingCpuMode, Mode);
}
