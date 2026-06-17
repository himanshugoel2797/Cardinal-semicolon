// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
#ifndef PRIV_ACPI_SPCR_DRIVER_H
#define PRIV_ACPI_SPCR_DRIVER_H

#include <stdint.h>
#include <types.h>

#include "acpi/acpi_tables.h"

/**
 * \addtogroup acpi_tables ACPI Tables
 * @{
 */

#define SPCR_SIG "SPCR" //!< Serial Port Console Redirection table

//! Serial Port Console Redirection table. Only the fields up to the base-address
//! descriptor are needed to locate the firmware console UART; the trailing fields
//! are declared so the struct's offsets stay correct.
typedef struct {
    ACPISDTHeader h;
    uint8_t interface_type; // 0 = full 16550, 1 = 16450 subset, 0x12 = 16550 (DBG2), ...
    uint8_t reserved0[3];
    GenericAddressStructure base_address; // .reserved byte carries the ACPI AccessSize
    uint8_t interrupt_type;
    uint8_t irq;
    uint32_t gsi;
    uint8_t baud_rate;
    uint8_t parity;
    uint8_t stop_bits;
    uint8_t flow_control;
    uint8_t terminal_type;
    uint8_t reserved1;
    uint16_t pci_device_id;
    uint16_t pci_vendor_id;
} PACKED SPCR;

//! GenericAddressStructure.address_space_id values
#define ACPI_GAS_SPACE_MEM 0 //!< System memory (MMIO)
#define ACPI_GAS_SPACE_IO 1  //!< System I/O (x86 port I/O)

//! GenericAddressStructure access size (its 4th byte, named `reserved` there)
#define ACPI_GAS_ACCESS_BYTE 1
#define ACPI_GAS_ACCESS_WORD 2
#define ACPI_GAS_ACCESS_DWORD 3
#define ACPI_GAS_ACCESS_QWORD 4

/**@}*/

#endif /* end of include guard */
