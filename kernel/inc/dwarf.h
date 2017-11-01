// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
#ifndef CARDINAL_ELF_DWARF_H
#define CARDINAL_ELF_DWARF_H

#include <stdint.h>

typedef struct PACKED {
    uint32_t length;
    uint16_t version;
    uint32_t debug_abbrev_offset;
    uint8_t address_size;
} DWARF32_CU;

typedef enum {
    DW_TAG_array_type = 0x01,
    DW_TAG_class_type = 0x02,
    DW_TAG_entry_point = 0x03,
    DW_TAG_enumeration_type = 0x04,
    DW_TAG_formal_parameter = 0x05,
    DW_TAG_imported_declaration = 0x08,
    DW_TAG_label = 0x0a,
    DW_TAG_lexical_block = 0x0b,
    DW_TAG_member = 0x0d,
    DW_TAG_pointer_type = 0x0f,
    DW_TAG_reference_type = 0x10,
    DW_TAG_compile_unit = 0x11,
    DW_TAG_string_type = 0x12,
    DW_TAG_structure_type = 0x13,
    DW_TAG_subroutine_type = 0x15,
    DW_TAG_typedef = 0x16,
} DW_TAG;

#endif