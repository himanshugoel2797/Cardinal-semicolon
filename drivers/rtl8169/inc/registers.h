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
#define PHYAR_REG (0x60)        // indirect MII (PHY) access
#define CPLUS_CMD_REG (0xE0)    // "C+" command register (16-bit)

#define _93C56_UNLOCK ((1 << 7) | (1 << 6))
#define _93C56_LOCK ((0 << 7) | (0 << 6))

#define CMD_RST_VAL (1 << 4)
#define CMD_RX_EN (1 << 3)
#define CMD_TX_EN (1 << 2)

// TXCFG (0x40) carries the hardware revision in its high bits; mask + match
// against the known hwrev IDs. RTL8111G == RL_HWREV_8168G.
#define HWREV_MASK (0x7C800000)
#define HWREV_8168G (0x4C000000)

// PHYAR (0x60): (reg<<16) | (data & DATA) | BUSY; BUSY clears on write-done.
#define PHYAR_DATA (0x0000FFFF)
#define PHYAR_BUSY (0x80000000)

// C+ command (0xE0) bits. 8168G value = PCI_MRW|RXCSUM|MACSTAT_DIS|0x0001.
#define CPLUS_TXENB (0x0001)    // old chips only
#define CPLUS_RXENB (0x0002)    // old chips only
#define CPLUS_PCI_MRW (0x0008)
#define CPLUS_RXCSUM (0x0020)
#define CPLUS_VLANSTRIP (0x0040)
#define CPLUS_MACSTAT_DIS (0x0080)
#define CPLUS_8168G_VAL (CPLUS_PCI_MRW | CPLUS_RXCSUM | CPLUS_MACSTAT_DIS | 0x0001)

// RXCFG (0x44) base config, matching BSD RL_RXCFG_CONFIG:
//   FIFO threshold = none (7<<13) | RX max DMA = unlimited (7<<8) | bufsz (3<<11).
// On the 8168G the low bit of the (3<<11) field is the "EarlyOff-V2" bit, so
// this base implicitly enables it; BSD writes the same 0xFF00 on every chip.
#define RCR_BASE_CONFIG ((7 << 13) | (3 << 11) | (7 << 8))

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