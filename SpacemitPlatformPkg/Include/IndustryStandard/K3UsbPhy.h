/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Base.h>

// K3 UTMI settings shared by host and device cold initialization.
// PLL fields match Linux phy-k1-usb2.c's K3 profile for a 24 MHz input.
#define K3_USB2_RESET_MODE        0x04
#define K3_USB2_PLL_READY         BIT0
#define K3_USB2_PLL_DIVIDER       0x98
#define K3_USB2_PLL_24MHZ         (BIT15 | (1U << 13) | 0x1ec4U)
#define K3_USB2_CLOCKS_ENABLE     (BIT1 | BIT2 | BIT3)
#define K3_USB2_RESET_DEASSERT    (BIT5 | BIT6 | BIT7)
#define K3_USB2_LINE_TX_MODES     (BIT13 | BIT14)
#define K3_USB2_RUNNING_MODE     (K3_USB2_PLL_READY | K3_USB2_CLOCKS_ENABLE | \
                                  K3_USB2_RESET_DEASSERT | K3_USB2_LINE_TX_MODES)
#define K3_USB2_HSTXP_CONTROL     0x34
#define K3_USB2_HSTXP_RUNNING     (BIT2 | BIT3 | BIT4)
#define K3_USB2_TX_HOST_CONTROL   0x10
#define K3_USB2_DISC_AUTO_CLEAR   BIT2
// Retain the UsbHcdInitDxe vendor analog profile. These are not PLL fields.
#define K3_USB2_VENDOR_REG18      0x18
#define K3_USB2_VENDOR_REG18_CLEAR BIT0
#define K3_USB2_VENDOR_REGA4      0xa4
#define K3_USB2_VENDOR_REGA4_MASK 0x0fU
#define K3_USB2_VENDOR_REGA4_SET  (0x0cU | BIT4)
// K3 combo PHY power override used to park the two USB3 orientations.
#define K3_COMBO_POWER_SELECT    0x40
#define K3_COMBO_POWER_STATUS    BIT9
#define K3_COMBO_POWER_OVERRIDE  BIT10
