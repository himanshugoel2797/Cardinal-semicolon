// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
#ifndef HPET_ACPI_TABLE_H
#define HPET_ACPI_TABLE_H

#include "acpi_tables.h"
#include <types.h>

typedef struct {
  ACPISDTHeader h;
  uint8_t RevisionID;
  uint8_t ComparatorCount : 5;
  uint8_t CounterIs64Bit : 1;
  uint8_t Rsv0 : 1;
  uint8_t LegacyReplacement : 1;
  uint16_t VendorID;
  GenericAddressStructure Address;
  uint8_t HPETNumber;
  uint16_t MinimumTick;
  uint8_t PageProtection;
} PACKED HPET;

#endif