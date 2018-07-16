
/******************************************************************************
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2005 - 2014 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110,
 * USA
 *
 * The full GNU General Public License is included in this distribution
 * in the file called COPYING.
 *
 * Contact Information:
 *  Intel Linux Wireless <ilw@linux.intel.com>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 *
 * BSD LICENSE
 *
 * Copyright(c) 2005 - 2014 Intel Corporation. All rights reserved.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************//*
 * BEGIN iwl-csr.h
 */

/*
 * CSR (control and status registers)
 *
 * CSR registers are mapped directly into PCI bus space, and are accessible
 * whenever platform supplies power to device, even when device is in
 * low power states due to driver-invoked device resets
 * (e.g. IWM_CSR_RESET_REG_FLAG_SW_RESET) or uCode-driven power-saving modes.
 *
 * Use iwl_write32() and iwl_read32() family to access these registers;
 * these provide simple PCI bus access, without waking up the MAC.
 * Do not use iwl_write_direct32() family for these registers;
 * no need to "grab nic access" via IWM_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ.
 * The MAC (uCode processor, etc.) does not need to be powered up for accessing
 * the CSR registers.
 *
 * NOTE:  Device does need to be awake in order to read this memory
 *        via IWM_CSR_EEPROM and IWM_CSR_OTP registers
 */
#define IWM_CSR_HW_IF_CONFIG_REG    (0x000) /* hardware interface config */
#define IWM_CSR_INT_COALESCING      (0x004) /* accum ints, 32-usec units */
#define IWM_CSR_INT                 (0x008) /* host interrupt status/ack */
#define IWM_CSR_INT_MASK            (0x00c) /* host interrupt enable */
#define IWM_CSR_FH_INT_STATUS       (0x010) /* busmaster int status/ack*/
#define IWM_CSR_GPIO_IN             (0x018) /* read external chip pins */
#define IWM_CSR_RESET               (0x020) /* busmaster enable, NMI, etc*/
#define IWM_CSR_GP_CNTRL            (0x024)

/* 2nd byte of IWM_CSR_INT_COALESCING, not accessible via iwl_write32()! */
#define IWM_CSR_INT_PERIODIC_REG	(0x005)

/*
 * Hardware revision info
 * Bit fields:
 * 31-16:  Reserved
 *  15-4:  Type of device:  see IWM_CSR_HW_REV_TYPE_xxx definitions
 *  3-2:  Revision step:  0 = A, 1 = B, 2 = C, 3 = D
 *  1-0:  "Dash" (-) value, as in A-1, etc.
 */
#define IWM_CSR_HW_REV              (0x028)

/*
 * EEPROM and OTP (one-time-programmable) memory reads
 *
 * NOTE:  Device must be awake, initialized via apm_ops.init(),
 *        in order to read.
 */
#define IWM_CSR_EEPROM_REG          (0x02c)
#define IWM_CSR_EEPROM_GP           (0x030)
#define IWM_CSR_OTP_GP_REG          (0x034)

#define IWM_CSR_GIO_REG		(0x03C)
#define IWM_CSR_GP_UCODE_REG	(0x048)
#define IWM_CSR_GP_DRIVER_REG	(0x050)

/*
 * UCODE-DRIVER GP (general purpose) mailbox registers.
 * SET/CLR registers set/clear bit(s) if "1" is written.
 */
#define IWM_CSR_UCODE_DRV_GP1       (0x054)
#define IWM_CSR_UCODE_DRV_GP1_SET   (0x058)
#define IWM_CSR_UCODE_DRV_GP1_CLR   (0x05c)
#define IWM_CSR_UCODE_DRV_GP2       (0x060)

#define IWM_CSR_MBOX_SET_REG		(0x088)
#define IWM_CSR_MBOX_SET_REG_OS_ALIVE	0x20

#define IWM_CSR_LED_REG			(0x094)
#define IWM_CSR_DRAM_INT_TBL_REG	(0x0A0)
#define IWM_CSR_MAC_SHADOW_REG_CTRL	(0x0A8) /* 6000 and up */


/* GIO Chicken Bits (PCI Express bus link power management) */
#define IWM_CSR_GIO_CHICKEN_BITS    (0x100)

/* Analog phase-lock-loop configuration  */
#define IWM_CSR_ANA_PLL_CFG         (0x20c)

/*
 * CSR Hardware Revision Workaround Register.  Indicates hardware rev;
 * "step" determines CCK backoff for txpower calculation.  Used for 4965 only.
 * See also IWM_CSR_HW_REV register.
 * Bit fields:
 *  3-2:  0 = A, 1 = B, 2 = C, 3 = D step
 *  1-0:  "Dash" (-) value, as in C-1, etc.
 */
#define IWM_CSR_HW_REV_WA_REG		(0x22C)

#define IWM_CSR_DBG_HPET_MEM_REG	(0x240)
#define IWM_CSR_DBG_LINK_PWR_MGMT_REG	(0x250)

/* Bits for IWM_CSR_HW_IF_CONFIG_REG */
#define IWM_CSR_HW_IF_CONFIG_REG_MSK_MAC_DASH	(0x00000003)
#define IWM_CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP	(0x0000000C)
#define IWM_CSR_HW_IF_CONFIG_REG_MSK_BOARD_VER	(0x000000C0)
#define IWM_CSR_HW_IF_CONFIG_REG_BIT_MAC_SI	(0x00000100)
#define IWM_CSR_HW_IF_CONFIG_REG_BIT_RADIO_SI	(0x00000200)
#define IWM_CSR_HW_IF_CONFIG_REG_MSK_PHY_TYPE	(0x00000C00)
#define IWM_CSR_HW_IF_CONFIG_REG_MSK_PHY_DASH	(0x00003000)
#define IWM_CSR_HW_IF_CONFIG_REG_MSK_PHY_STEP	(0x0000C000)

#define IWM_CSR_HW_IF_CONFIG_REG_POS_MAC_DASH	(0)
#define IWM_CSR_HW_IF_CONFIG_REG_POS_MAC_STEP	(2)
#define IWM_CSR_HW_IF_CONFIG_REG_POS_BOARD_VER	(6)
#define IWM_CSR_HW_IF_CONFIG_REG_POS_PHY_TYPE	(10)
#define IWM_CSR_HW_IF_CONFIG_REG_POS_PHY_DASH	(12)
#define IWM_CSR_HW_IF_CONFIG_REG_POS_PHY_STEP	(14)

#define IWM_CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A	(0x00080000)
#define IWM_CSR_HW_IF_CONFIG_REG_BIT_EEPROM_OWN_SEM	(0x00200000)
#define IWM_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY	(0x00400000) /* PCI_OWN_SEM */
#define IWM_CSR_HW_IF_CONFIG_REG_BIT_NIC_PREPARE_DONE (0x02000000) /* ME_OWN */
#define IWM_CSR_HW_IF_CONFIG_REG_PREPARE	(0x08000000) /* WAKE_ME */
#define IWM_CSR_HW_IF_CONFIG_REG_ENABLE_PME	(0x10000000)
#define IWM_CSR_HW_IF_CONFIG_REG_PERSIST_MODE	(0x40000000) /* PERSISTENCE */

#define IWM_CSR_INT_PERIODIC_DIS		(0x00) /* disable periodic int*/
#define IWM_CSR_INT_PERIODIC_ENA		(0xFF) /* 255*32 usec ~ 8 msec*/

/* interrupt flags in INTA, set by uCode or hardware (e.g. dma),
 * acknowledged (reset) by host writing "1" to flagged bits. */
#define IWM_CSR_INT_BIT_FH_RX	(1 << 31) /* Rx DMA, cmd responses, FH_INT[17:16] */
#define IWM_CSR_INT_BIT_HW_ERR	(1 << 29) /* DMA hardware error FH_INT[31] */
#define IWM_CSR_INT_BIT_RX_PERIODIC	(1 << 28) /* Rx periodic */
#define IWM_CSR_INT_BIT_FH_TX	(1 << 27) /* Tx DMA FH_INT[1:0] */
#define IWM_CSR_INT_BIT_SCD	(1 << 26) /* TXQ pointer advanced */
#define IWM_CSR_INT_BIT_SW_ERR	(1 << 25) /* uCode error */
#define IWM_CSR_INT_BIT_RF_KILL	(1 << 7)  /* HW RFKILL switch GP_CNTRL[27] toggled */
#define IWM_CSR_INT_BIT_CT_KILL	(1 << 6)  /* Critical temp (chip too hot) rfkill */
#define IWM_CSR_INT_BIT_SW_RX	(1 << 3)  /* Rx, command responses */
#define IWM_CSR_INT_BIT_WAKEUP	(1 << 1)  /* NIC controller waking up (pwr mgmt) */
#define IWM_CSR_INT_BIT_ALIVE	(1 << 0)  /* uCode interrupts once it initializes */

#define IWM_CSR_INI_SET_MASK	(IWM_CSR_INT_BIT_FH_RX   | \
				 IWM_CSR_INT_BIT_HW_ERR  | \
				 IWM_CSR_INT_BIT_FH_TX   | \
				 IWM_CSR_INT_BIT_SW_ERR  | \
				 IWM_CSR_INT_BIT_RF_KILL | \
				 IWM_CSR_INT_BIT_SW_RX   | \
				 IWM_CSR_INT_BIT_WAKEUP  | \
				 IWM_CSR_INT_BIT_ALIVE   | \
				 IWM_CSR_INT_BIT_RX_PERIODIC)

/* interrupt flags in FH (flow handler) (PCI busmaster DMA) */
#define IWM_CSR_FH_INT_BIT_ERR       (1 << 31) /* Error */
#define IWM_CSR_FH_INT_BIT_HI_PRIOR  (1 << 30) /* High priority Rx, bypass coalescing */
#define IWM_CSR_FH_INT_BIT_RX_CHNL1  (1 << 17) /* Rx channel 1 */
#define IWM_CSR_FH_INT_BIT_RX_CHNL0  (1 << 16) /* Rx channel 0 */
#define IWM_CSR_FH_INT_BIT_TX_CHNL1  (1 << 1)  /* Tx channel 1 */
#define IWM_CSR_FH_INT_BIT_TX_CHNL0  (1 << 0)  /* Tx channel 0 */

#define IWM_CSR_FH_INT_RX_MASK	(IWM_CSR_FH_INT_BIT_HI_PRIOR | \
				IWM_CSR_FH_INT_BIT_RX_CHNL1 | \
				IWM_CSR_FH_INT_BIT_RX_CHNL0)

#define IWM_CSR_FH_INT_TX_MASK	(IWM_CSR_FH_INT_BIT_TX_CHNL1 | \
				IWM_CSR_FH_INT_BIT_TX_CHNL0)

/* GPIO */
#define IWM_CSR_GPIO_IN_BIT_AUX_POWER                   (0x00000200)
#define IWM_CSR_GPIO_IN_VAL_VAUX_PWR_SRC                (0x00000000)
#define IWM_CSR_GPIO_IN_VAL_VMAIN_PWR_SRC               (0x00000200)

/* RESET */
#define IWM_CSR_RESET_REG_FLAG_NEVO_RESET                (0x00000001)
#define IWM_CSR_RESET_REG_FLAG_FORCE_NMI                 (0x00000002)
#define IWM_CSR_RESET_REG_FLAG_SW_RESET                  (0x00000080)
#define IWM_CSR_RESET_REG_FLAG_MASTER_DISABLED           (0x00000100)
#define IWM_CSR_RESET_REG_FLAG_STOP_MASTER               (0x00000200)
#define IWM_CSR_RESET_LINK_PWR_MGMT_DISABLED             (0x80000000)

/*
 * GP (general purpose) CONTROL REGISTER
 * Bit fields:
 *    27:  HW_RF_KILL_SW
 *         Indicates state of (platform's) hardware RF-Kill switch
 * 26-24:  POWER_SAVE_TYPE
 *         Indicates current power-saving mode:
 *         000 -- No power saving
 *         001 -- MAC power-down
 *         010 -- PHY (radio) power-down
 *         011 -- Error
 *   9-6:  SYS_CONFIG
 *         Indicates current system configuration, reflecting pins on chip
 *         as forced high/low by device circuit board.
 *     4:  GOING_TO_SLEEP
 *         Indicates MAC is entering a power-saving sleep power-down.
 *         Not a good time to access device-internal resources.
 *     3:  MAC_ACCESS_REQ
 *         Host sets this to request and maintain MAC wakeup, to allow host
 *         access to device-internal resources.  Host must wait for
 *         MAC_CLOCK_READY (and !GOING_TO_SLEEP) before accessing non-CSR
 *         device registers.
 *     2:  INIT_DONE
 *         Host sets this to put device into fully operational D0 power mode.
 *         Host resets this after SW_RESET to put device into low power mode.
 *     0:  MAC_CLOCK_READY
 *         Indicates MAC (ucode processor, etc.) is powered up and can run.
 *         Internal resources are accessible.
 *         NOTE:  This does not indicate that the processor is actually running.
 *         NOTE:  This does not indicate that device has completed
 *                init or post-power-down restore of internal SRAM memory.
 *                Use IWM_CSR_UCODE_DRV_GP1_BIT_MAC_SLEEP as indication that
 *                SRAM is restored and uCode is in normal operation mode.
 *                Later devices (5xxx/6xxx/1xxx) use non-volatile SRAM, and
 *                do not need to save/restore it.
 *         NOTE:  After device reset, this bit remains "0" until host sets
 *                INIT_DONE
 */
#define IWM_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY        (0x00000001)
#define IWM_CSR_GP_CNTRL_REG_FLAG_INIT_DONE              (0x00000004)
#define IWM_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ         (0x00000008)
#define IWM_CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP         (0x00000010)

#define IWM_CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN           (0x00000001)

#define IWM_CSR_GP_CNTRL_REG_MSK_POWER_SAVE_TYPE         (0x07000000)
#define IWM_CSR_GP_CNTRL_REG_FLAG_MAC_POWER_SAVE         (0x04000000)
#define IWM_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW          (0x08000000)


/* HW REV */
#define IWM_CSR_HW_REV_DASH(_val)          (((_val) & 0x0000003) >> 0)
#define IWM_CSR_HW_REV_STEP(_val)          (((_val) & 0x000000C) >> 2)

/**
 *  hw_rev values
 */
enum {
	IWM_SILICON_A_STEP = 0,
	IWM_SILICON_B_STEP,
	IWM_SILICON_C_STEP,
};


#define IWM_CSR_HW_REV_TYPE_MSK		(0x000FFF0)
#define IWM_CSR_HW_REV_TYPE_5300	(0x0000020)
#define IWM_CSR_HW_REV_TYPE_5350	(0x0000030)
#define IWM_CSR_HW_REV_TYPE_5100	(0x0000050)
#define IWM_CSR_HW_REV_TYPE_5150	(0x0000040)
#define IWM_CSR_HW_REV_TYPE_1000	(0x0000060)
#define IWM_CSR_HW_REV_TYPE_6x00	(0x0000070)
#define IWM_CSR_HW_REV_TYPE_6x50	(0x0000080)
#define IWM_CSR_HW_REV_TYPE_6150	(0x0000084)
#define IWM_CSR_HW_REV_TYPE_6x05	(0x00000B0)
#define IWM_CSR_HW_REV_TYPE_6x30	IWM_CSR_HW_REV_TYPE_6x05
#define IWM_CSR_HW_REV_TYPE_6x35	IWM_CSR_HW_REV_TYPE_6x05
#define IWM_CSR_HW_REV_TYPE_2x30	(0x00000C0)
#define IWM_CSR_HW_REV_TYPE_2x00	(0x0000100)
#define IWM_CSR_HW_REV_TYPE_105		(0x0000110)
#define IWM_CSR_HW_REV_TYPE_135		(0x0000120)
#define IWM_CSR_HW_REV_TYPE_7265D	(0x0000210)
#define IWM_CSR_HW_REV_TYPE_NONE	(0x00001F0)

/* EEPROM REG */
#define IWM_CSR_EEPROM_REG_READ_VALID_MSK	(0x00000001)
#define IWM_CSR_EEPROM_REG_BIT_CMD		(0x00000002)
#define IWM_CSR_EEPROM_REG_MSK_ADDR		(0x0000FFFC)
#define IWM_CSR_EEPROM_REG_MSK_DATA		(0xFFFF0000)

/* EEPROM GP */
#define IWM_CSR_EEPROM_GP_VALID_MSK		(0x00000007) /* signature */
#define IWM_CSR_EEPROM_GP_IF_OWNER_MSK	(0x00000180)
#define IWM_CSR_EEPROM_GP_BAD_SIGNATURE_BOTH_EEP_AND_OTP	(0x00000000)
#define IWM_CSR_EEPROM_GP_BAD_SIG_EEP_GOOD_SIG_OTP		(0x00000001)
#define IWM_CSR_EEPROM_GP_GOOD_SIG_EEP_LESS_THAN_4K		(0x00000002)
#define IWM_CSR_EEPROM_GP_GOOD_SIG_EEP_MORE_THAN_4K		(0x00000004)

/* One-time-programmable memory general purpose reg */
#define IWM_CSR_OTP_GP_REG_DEVICE_SELECT  (0x00010000) /* 0 - EEPROM, 1 - OTP */
#define IWM_CSR_OTP_GP_REG_OTP_ACCESS_MODE  (0x00020000) /* 0 - absolute, 1 - relative */
#define IWM_CSR_OTP_GP_REG_ECC_CORR_STATUS_MSK    (0x00100000) /* bit 20 */
#define IWM_CSR_OTP_GP_REG_ECC_UNCORR_STATUS_MSK  (0x00200000) /* bit 21 */

/* GP REG */
#define IWM_CSR_GP_REG_POWER_SAVE_STATUS_MSK    (0x03000000) /* bit 24/25 */
#define IWM_CSR_GP_REG_NO_POWER_SAVE            (0x00000000)
#define IWM_CSR_GP_REG_MAC_POWER_SAVE           (0x01000000)
#define IWM_CSR_GP_REG_PHY_POWER_SAVE           (0x02000000)
#define IWM_CSR_GP_REG_POWER_SAVE_ERROR         (0x03000000)


/* CSR GIO */
#define IWM_CSR_GIO_REG_VAL_L0S_ENABLED	(0x00000002)

/*
 * UCODE-DRIVER GP (general purpose) mailbox register 1
 * Host driver and uCode write and/or read this register to communicate with
 * each other.
 * Bit fields:
 *     4:  UCODE_DISABLE
 *         Host sets this to request permanent halt of uCode, same as
 *         sending CARD_STATE command with "halt" bit set.
 *     3:  CT_KILL_EXIT
 *         Host sets this to request exit from CT_KILL state, i.e. host thinks
 *         device temperature is low enough to continue normal operation.
 *     2:  CMD_BLOCKED
 *         Host sets this during RF KILL power-down sequence (HW, SW, CT KILL)
 *         to release uCode to clear all Tx and command queues, enter
 *         unassociated mode, and power down.
 *         NOTE:  Some devices also use HBUS_TARG_MBX_C register for this bit.
 *     1:  SW_BIT_RFKILL
 *         Host sets this when issuing CARD_STATE command to request
 *         device sleep.
 *     0:  MAC_SLEEP
 *         uCode sets this when preparing a power-saving power-down.
 *         uCode resets this when power-up is complete and SRAM is sane.
 *         NOTE:  device saves internal SRAM data to host when powering down,
 *                and must restore this data after powering back up.
 *                MAC_SLEEP is the best indication that restore is complete.
 *                Later devices (5xxx/6xxx/1xxx) use non-volatile SRAM, and
 *                do not need to save/restore it.
 */
#define IWM_CSR_UCODE_DRV_GP1_BIT_MAC_SLEEP             (0x00000001)
#define IWM_CSR_UCODE_SW_BIT_RFKILL                     (0x00000002)
#define IWM_CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED           (0x00000004)
#define IWM_CSR_UCODE_DRV_GP1_REG_BIT_CT_KILL_EXIT      (0x00000008)
#define IWM_CSR_UCODE_DRV_GP1_BIT_D3_CFG_COMPLETE       (0x00000020)

/* GP Driver */
#define IWM_CSR_GP_DRIVER_REG_BIT_RADIO_SKU_MSK		    (0x00000003)
#define IWM_CSR_GP_DRIVER_REG_BIT_RADIO_SKU_3x3_HYB	    (0x00000000)
#define IWM_CSR_GP_DRIVER_REG_BIT_RADIO_SKU_2x2_HYB	    (0x00000001)
#define IWM_CSR_GP_DRIVER_REG_BIT_RADIO_SKU_2x2_IPA	    (0x00000002)
#define IWM_CSR_GP_DRIVER_REG_BIT_CALIB_VERSION6	    (0x00000004)
#define IWM_CSR_GP_DRIVER_REG_BIT_6050_1x2		    (0x00000008)

#define IWM_CSR_GP_DRIVER_REG_BIT_RADIO_IQ_INVER	    (0x00000080)

/* GIO Chicken Bits (PCI Express bus link power management) */
#define IWM_CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX  (0x00800000)
#define IWM_CSR_GIO_CHICKEN_BITS_REG_BIT_DIS_L0S_EXIT_TIMER  (0x20000000)

/* LED */
#define IWM_CSR_LED_BSM_CTRL_MSK (0xFFFFFFDF)
#define IWM_CSR_LED_REG_TURN_ON (0x60)
#define IWM_CSR_LED_REG_TURN_OFF (0x20)

/* ANA_PLL */
#define IWM_CSR50_ANA_PLL_CFG_VAL        (0x00880300)

/* HPET MEM debug */
#define IWM_CSR_DBG_HPET_MEM_REG_VAL	(0xFFFF0000)

/* DRAM INT TABLE */
#define IWM_CSR_DRAM_INT_TBL_ENABLE		(1 << 31)
#define IWM_CSR_DRAM_INIT_TBL_WRITE_POINTER	(1 << 28)
#define IWM_CSR_DRAM_INIT_TBL_WRAP_CHECK	(1 << 27)

/* SECURE boot registers */
#define IWM_CSR_SECURE_BOOT_CONFIG_ADDR	(0x100)
enum iwm_secure_boot_config_reg {
	IWM_CSR_SECURE_BOOT_CONFIG_INSPECTOR_BURNED_IN_OTP	= 0x00000001,
	IWM_CSR_SECURE_BOOT_CONFIG_INSPECTOR_NOT_REQ	= 0x00000002,
};

#define IWM_CSR_SECURE_BOOT_CPU1_STATUS_ADDR	(0x100)
#define IWM_CSR_SECURE_BOOT_CPU2_STATUS_ADDR	(0x100)
enum iwm_secure_boot_status_reg {
	IWM_CSR_SECURE_BOOT_CPU_STATUS_VERF_STATUS		= 0x00000003,
	IWM_CSR_SECURE_BOOT_CPU_STATUS_VERF_COMPLETED	= 0x00000002,
	IWM_CSR_SECURE_BOOT_CPU_STATUS_VERF_SUCCESS		= 0x00000004,
	IWM_CSR_SECURE_BOOT_CPU_STATUS_VERF_FAIL		= 0x00000008,
	IWM_CSR_SECURE_BOOT_CPU_STATUS_SIGN_VERF_FAIL	= 0x00000010,
};

#define IWM_FH_UCODE_LOAD_STATUS	0x1af0
#define IWM_FH_MEM_TB_MAX_LENGTH	0x20000

#define IWM_LMPM_SECURE_UCODE_LOAD_CPU1_HDR_ADDR	0x1e78
#define IWM_LMPM_SECURE_UCODE_LOAD_CPU2_HDR_ADDR	0x1e7c

#define IWM_LMPM_SECURE_CPU1_HDR_MEM_SPACE		0x420000
#define IWM_LMPM_SECURE_CPU2_HDR_MEM_SPACE		0x420400

#define IWM_CSR_SECURE_TIME_OUT	(100)

/* extended range in FW SRAM */
#define IWM_FW_MEM_EXTENDED_START       0x40000
#define IWM_FW_MEM_EXTENDED_END         0x57FFF

/* FW chicken bits */
#define IWM_LMPM_CHICK				0xa01ff8
#define IWM_LMPM_CHICK_EXTENDED_ADDR_SPACE	0x01

#define IWM_FH_TCSR_0_REG0 (0x1D00)

/*
 * HBUS (Host-side Bus)
 *
 * HBUS registers are mapped directly into PCI bus space, but are used
 * to indirectly access device's internal memory or registers that
 * may be powered-down.
 *
 * Use iwl_write_direct32()/iwl_read_direct32() family for these registers;
 * host must "grab nic access" via CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ
 * to make sure the MAC (uCode processor, etc.) is powered up for accessing
 * internal resources.
 *
 * Do not use iwl_write32()/iwl_read32() family to access these registers;
 * these provide only simple PCI bus access, without waking up the MAC.
 */
#define IWM_HBUS_BASE	(0x400)

/*
 * Registers for accessing device's internal SRAM memory (e.g. SCD SRAM
 * structures, error log, event log, verifying uCode load).
 * First write to address register, then read from or write to data register
 * to complete the job.  Once the address register is set up, accesses to
 * data registers auto-increment the address by one dword.
 * Bit usage for address registers (read or write):
 *  0-31:  memory address within device
 */
#define IWM_HBUS_TARG_MEM_RADDR     (IWM_HBUS_BASE+0x00c)
#define IWM_HBUS_TARG_MEM_WADDR     (IWM_HBUS_BASE+0x010)
#define IWM_HBUS_TARG_MEM_WDAT      (IWM_HBUS_BASE+0x018)
#define IWM_HBUS_TARG_MEM_RDAT      (IWM_HBUS_BASE+0x01c)

/* Mailbox C, used as workaround alternative to CSR_UCODE_DRV_GP1 mailbox */
#define IWM_HBUS_TARG_MBX_C         (IWM_HBUS_BASE+0x030)
#define IWM_HBUS_TARG_MBX_C_REG_BIT_CMD_BLOCKED         (0x00000004)

/*
 * Registers for accessing device's internal peripheral registers
 * (e.g. SCD, BSM, etc.).  First write to address register,
 * then read from or write to data register to complete the job.
 * Bit usage for address registers (read or write):
 *  0-15:  register address (offset) within device
 * 24-25:  (# bytes - 1) to read or write (e.g. 3 for dword)
 */
#define IWM_HBUS_TARG_PRPH_WADDR    (IWM_HBUS_BASE+0x044)
#define IWM_HBUS_TARG_PRPH_RADDR    (IWM_HBUS_BASE+0x048)
#define IWM_HBUS_TARG_PRPH_WDAT     (IWM_HBUS_BASE+0x04c)
#define IWM_HBUS_TARG_PRPH_RDAT     (IWM_HBUS_BASE+0x050)

/* enable the ID buf for read */
#define IWM_WFPM_PS_CTL_CLR			0xa0300c
#define IWM_WFMP_MAC_ADDR_0			0xa03080
#define IWM_WFMP_MAC_ADDR_1			0xa03084
#define IWM_LMPM_PMG_EN				0xa01cec
#define IWM_RADIO_REG_SYS_MANUAL_DFT_0		0xad4078
#define IWM_RFIC_REG_RD				0xad0470
#define IWM_WFPM_CTRL_REG			0xa03030
#define IWM_WFPM_AUX_CTL_AUX_IF_MAC_OWNER_MSK	0x08000000
#define IWM_ENABLE_WFPM				0x80000000

#define IWM_AUX_MISC_REG			0xa200b0
#define IWM_HW_STEP_LOCATION_BITS		24

#define IWM_AUX_MISC_MASTER1_EN			0xa20818
#define IWM_AUX_MISC_MASTER1_EN_SBE_MSK		0x1
#define IWM_AUX_MISC_MASTER1_SMPHR_STATUS	0xa20800
#define IWM_RSA_ENABLE				0xa24b08
#define IWM_PREG_AUX_BUS_WPROT_0		0xa04cc0
#define IWM_SB_CFG_OVERRIDE_ADDR		0xa26c78
#define IWM_SB_CFG_OVERRIDE_ENABLE		0x8000
#define IWM_SB_CFG_BASE_OVERRIDE		0xa20000
#define IWM_SB_MODIFY_CFG_FLAG			0xa03088
#define IWM_SB_CPU_1_STATUS			0xa01e30
#define IWM_SB_CPU_2_STATUS			0Xa01e34

/* Used to enable DBGM */
#define IWM_HBUS_TARG_TEST_REG	(IWM_HBUS_BASE+0x05c)

/*
 * Per-Tx-queue write pointer (index, really!)
 * Indicates index to next TFD that driver will fill (1 past latest filled).
 * Bit usage:
 *  0-7:  queue write index
 * 11-8:  queue selector
 */
#define IWM_HBUS_TARG_WRPTR         (IWM_HBUS_BASE+0x060)

/**********************************************************
 * CSR values
 **********************************************************/
 /*
 * host interrupt timeout value
 * used with setting interrupt coalescing timer
 * the CSR_INT_COALESCING is an 8 bit register in 32-usec unit
 *
 * default interrupt coalescing timer is 64 x 32 = 2048 usecs
 */
#define IWM_HOST_INT_TIMEOUT_MAX	(0xFF)
#define IWM_HOST_INT_TIMEOUT_DEF	(0x40)
#define IWM_HOST_INT_TIMEOUT_MIN	(0x0)
#define IWM_HOST_INT_OPER_MODE		(1 << 31)

/*****************************************************************************
 *                        7000/3000 series SHR DTS addresses                 *
 *****************************************************************************/

/* Diode Results Register Structure: */
enum iwm_dtd_diode_reg {
	IWM_DTS_DIODE_REG_DIG_VAL		= 0x000000FF, /* bits [7:0] */
	IWM_DTS_DIODE_REG_VREF_LOW		= 0x0000FF00, /* bits [15:8] */
	IWM_DTS_DIODE_REG_VREF_HIGH		= 0x00FF0000, /* bits [23:16] */
	IWM_DTS_DIODE_REG_VREF_ID		= 0x03000000, /* bits [25:24] */
	IWM_DTS_DIODE_REG_PASS_ONCE		= 0x80000000, /* bits [31:31] */
	IWM_DTS_DIODE_REG_FLAGS_MSK		= 0xFF000000, /* bits [31:24] */
/* Those are the masks INSIDE the flags bit-field: */
	IWM_DTS_DIODE_REG_FLAGS_VREFS_ID_POS	= 0,
	IWM_DTS_DIODE_REG_FLAGS_VREFS_ID	= 0x00000003, /* bits [1:0] */
	IWM_DTS_DIODE_REG_FLAGS_PASS_ONCE_POS	= 7,
	IWM_DTS_DIODE_REG_FLAGS_PASS_ONCE	= 0x00000080, /* bits [7:7] */
};

/*
 * END iwl-csr.h
 */
