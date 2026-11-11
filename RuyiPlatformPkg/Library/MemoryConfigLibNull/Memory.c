/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/MemoryConfigLib.h>

EFI_STATUS
MemoryConfigGet (BOOLEAN *EccEnabled)
{
  if (EccEnabled == NULL) { return EFI_INVALID_PARAMETER; }
  *EccEnabled = FALSE;
  return EFI_UNSUPPORTED;
}

EFI_STATUS
MemoryConfigSet (BOOLEAN EccEnabled)
{
  return EFI_UNSUPPORTED;
}
