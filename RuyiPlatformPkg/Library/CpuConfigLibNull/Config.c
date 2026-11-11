/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/CpuConfigLib.h>
EFI_STATUS CpuConfigGet (CPU_CONFIG *Config) { return Config ? EFI_UNSUPPORTED : EFI_INVALID_PARAMETER; }
EFI_STATUS CpuConfigSet (UINT8 Mode) { return EFI_UNSUPPORTED; }
