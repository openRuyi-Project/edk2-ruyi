/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once

#include <Base.h>

/** Return the CPU pointer used to access an MMIO physical address. */
UINTN
EFIAPI
CpuMmioAddress (
  IN UINTN  Address
  );
