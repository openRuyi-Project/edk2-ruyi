/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once

#include <Base.h>

/* EIC7702 places die 1 peripheral and cache registers 512 MiB above die 0. */
#define EIC7702_MMIO_DIE(Base)  (((Base) & BIT29) != 0)
