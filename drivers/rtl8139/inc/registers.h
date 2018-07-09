// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SEMI_RTL8139_DRIVER_H
#define CARDINAL_SEMI_RTL8139_DRIVER_H

#define MAC_REG(x) (x)
#define MAR_REG(x) (x + 0x08)
#define RBSTART_REG (0x30)
#define CMD_REG (0x37)
#define IMR_REG (0x3C)
#define ISR_REG (0x3E)
#define RCR_REG (0x44)
#define CONFIG_1_REG (0x52)

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