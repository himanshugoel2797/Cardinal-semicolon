// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SEMI_RTL8169_DRIVER_H
#define CARDINAL_SEMI_RTL8169_DRIVER_H

#define MAC_REG(x) (x)
#define MAR_REG(x) (x + 0x08)
#define RX_ADDR_REG (0xE4)
#define TX_ADDR_REG (0x20)
#define TX_CFG_REG (0x40)
#define CMD_REG (0x37)
#define IMR_REG (0x3C)
#define ISR_REG (0x3E)
#define RCR_REG (0x44)
#define MISSEDPKT_REG (0x4C)
#define _93C56_CMD (0x50)
#define CONFIG_1_REG (0x52)
#define CONFIG_2_REG (0x53)
#define MAX_RX_PACKET_SIZE_REG (0xDA)
#define MAX_TX_PACKET_SIZE_REG (0xEC)
#define PHY_STATUS_REG (0x6C)
#define MISC_REG (0xF0)

#define _93C56_UNLOCK ((1 << 7) | (1 << 6))
#define _93C56_LOCK ((0 << 7) | (0 << 6))

#define CMD_RST_VAL (1 << 4)
#define CMD_RX_EN (1 << 3)
#define CMD_TX_EN (1 << 2)

#define INTR_ROK (1 << 0)
#define INTR_TOK (1 << 2)
#define INTR_TIMEOUT (1 << 14)

#define RCR_RCV_ALL (1 << 0)
#define RCR_RCV_PHYSMATCH (1 << 1)
#define RCR_RCV_MULTICAST (1 << 2)
#define RCR_RCV_BROADCAST (1 << 3)

#define RCR_WRAP (1 << 7)
#define RCR_RX_BUFLEN_64K (3 << 11)

#endif