/** SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Base.h>

// DWMAC 5.40a MAC, MTL queue 0 and DMA channel 0 register fields.
#define MAC_CONFIG                   0x0000
#define MAC_RXQ_CONTROL0             0x00a0
#define MAC_HW_FEATURE1              0x0120
#define MAC_MDIO_ADDRESS             0x0200
#define MAC_MDIO_DATA                0x0204
#define MAC_ADDRESS0_HIGH            0x0300
#define MAC_ADDRESS0_LOW             0x0304
#define MTL_TXQ_OPERATION            0x0d00
#define MTL_RXQ_OPERATION            0x0d30
#define DMA_MODE                     0x1000
#define DMA_SYSBUS_MODE              0x1004
#define DMA_CHANNEL_CONTROL          0x1100
#define DMA_TX_CONTROL               0x1104
#define DMA_RX_CONTROL               0x1108
#define DMA_TX_DESC_HIGH             0x1110
#define DMA_TX_DESC_LOW              0x1114
#define DMA_RX_DESC_HIGH             0x1118
#define DMA_RX_DESC_LOW              0x111c
#define DMA_TX_TAIL                  0x1120
#define DMA_RX_TAIL                  0x1128
#define DMA_TX_RING_LENGTH           0x112c
#define DMA_RX_RING_LENGTH           0x1130
#define DMA_INTERRUPT_ENABLE         0x1134

#define MAC_ADDRESS_ENABLE           BIT31
#define MAC_CONFIG_RECEIVE           BIT0
#define MAC_CONFIG_TRANSMIT          BIT1
#define MAC_CONFIG_FULL_DUPLEX       BIT13
#define MAC_CONFIG_FAST_ETHERNET     BIT14
#define MAC_CONFIG_MII_PORT          BIT15
#define MAC_CONFIG_AUTO_PAD_CRC      BIT20
#define MAC_CONFIG_CRC_TYPE          BIT21
#define MAC_TX_FIFO_SHIFT            6
#define MAC_FIFO_SIZE_MASK           0x1fU
#define MAC_RXQ_ENABLE_MASK          3U
#define MAC_RXQ_DCB_ENABLE           2U
#define MDIO_PHY_SHIFT               21
#define MDIO_REGISTER_SHIFT          16
#define MDIO_CLOCK_SHIFT             8
#define MDIO_WRITE                   (1U << 2)
#define MDIO_READ                    (3U << 2)
#define MDIO_BUSY                    BIT0
#define MTL_TXQ_SIZE_SHIFT           16
#define MTL_TXQ_SIZE_MASK            (0x1ffU << MTL_TXQ_SIZE_SHIFT)
#define MTL_TXQ_ENABLE_MASK          (3U << 2)
#define MTL_TXQ_ENABLE               (2U << 2)
#define MTL_TXQ_STORE_FORWARD        BIT1
#define MTL_RXQ_SIZE_SHIFT           20
#define MTL_RXQ_SIZE_MASK            (0x3ffU << MTL_RXQ_SIZE_SHIFT)
#define MTL_RXQ_STORE_FORWARD        BIT5
#define DMA_SOFT_RESET               BIT0
#define DMA_DESCRIPTOR_SKIP_MASK     (7U << 18)
#define DMA_PBL_X8                   BIT16
#define DMA_PBL_SHIFT                16
#define DMA_PBL_MASK                 (0x3fU << DMA_PBL_SHIFT)
#define DMA_TX_SECOND_PACKET         BIT4
#define DMA_RX_BUFFER_SHIFT          1
#define DMA_RX_BUFFER_MASK           (0x3fffU << DMA_RX_BUFFER_SHIFT)
#define DMA_START                    BIT0
#define RX_DESCRIPTOR_BUFFER1_VALID  BIT24
#define RX_DESCRIPTOR_CONTEXT        BIT30
#define RX_DESCRIPTOR_LENGTH_MASK    0x7fffU
// Existing firmware burst length, in bus beats before the PBL multiplier.
#define DMA_BURST_LENGTH             8U
