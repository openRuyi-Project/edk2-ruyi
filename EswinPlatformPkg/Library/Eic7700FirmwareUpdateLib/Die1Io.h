/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once

// Shared with the position-independent secondary-hart routine.
#define D560_REQ       0
#define D560_ACK       4
#define D560_READY     8
#define D560_STOP      12
#define D560_REGION    16
#define D560_OFFSET    20
#define D560_WORDS     24
#define D560_WRITE     28
#define D560_BUFFER    32
#define D560_REGISTERS 0
#define D560_WINDOW    1
#define D560_CLOCKS    2
#define D560_SPI1      0x71800000
#define D560_SPI1_DATA 0x7c000000
#define D560_SPI1_CRG  0x71828000
#define D560_SBI_HSM   0x48534d
