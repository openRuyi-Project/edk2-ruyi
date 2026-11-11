/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MEMORY_CONFIG_LIB_H
#define MEMORY_CONFIG_LIB_H
#include <Uefi.h>

// EFI_UNSUPPORTED means that this platform has no selectable ECC policy.
EFI_STATUS MemoryConfigGet (BOOLEAN *EccEnabled);
// Save the policy for the next full platform restart.
EFI_STATUS MemoryConfigSet (BOOLEAN EccEnabled);

#endif
