/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Base.h>
// EIC7700 pinctrl register encoding. Offsets are bytes from the DT resource.
#define EIC7700_PIN_STRIDE             sizeof (UINT32)
#define EIC7700_PIN_FUNCTION_MASK      (7U << 16)
#define EIC7700_PIN_INPUT_ENABLE       BIT0
#define EIC7700_PIN_PULL_MASK          (BIT1 | BIT2)
#define EIC7700_PIN_MODE_SET0           0x04U
#define EIC7700_PIN_MODE_SET0_GPIO      (2U << 16)
#define EIC7700_PIN_GPIO106            (153U * EIC7700_PIN_STRIDE)
#define EIC7700_PIN_GPIO111            (158U * EIC7700_PIN_STRIDE)
#define EIC7700_PIN_GPIO_DEFAULT       0U
#define EIC7700_GPIO_BANKS             4U
#define EIC7700_GPIO_PINS_PER_BANK     32U
