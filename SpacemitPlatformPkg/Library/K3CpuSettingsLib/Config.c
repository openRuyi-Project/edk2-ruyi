/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/CpuConfigLib.h>
#include <Library/K3CpuConfigLib.h>

STATIC CONST CPU_CONFIG_MODE mModes[] = {
  { K3CpuX100Only, L"X100 only (8 cores)" },
  { K3CpuX100A100, L"X100 + A100 (16 cores, no vector)" },
  { K3CpuA100Only, L"A100 only (8 cores, vector enabled)" }
};

EFI_STATUS
CpuConfigGet (CPU_CONFIG *Config)
{
  UINT32 Mode;
  EFI_STATUS Status;
  if (Config == NULL) { return EFI_INVALID_PARAMETER; }
  Status = K3CpuConfigGet (&Mode);
  if (EFI_ERROR (Status)) { return Status; }
  *Config = (CPU_CONFIG) {
    (UINT8)Mode, mModes, ARRAY_SIZE (mModes),
    L"X100 only retains the original CPU features. Mixed mode disables vector and hypervisor extensions. A100 only enables vectors and has no hypervisor extension. Save and restart to apply."
  };
  return EFI_SUCCESS;
}

EFI_STATUS CpuConfigSet (UINT8 Mode) { return K3CpuConfigSet (Mode); }
