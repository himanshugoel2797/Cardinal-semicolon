/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "initrd.h"
#include "boot_information.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

typedef struct TARHeader {
    char filename[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag[1];
} TARHeader;

unsigned int getsize(const char *in) {

    unsigned int size = 0;
    unsigned int j;
    unsigned int count = 1;

    for (j = 11; j > 0; j--, count *= 8)
        size += ((in[j - 1] - '0') * count);

    return size;
}

bool
Initrd_GetFile(const char *file,
               void **loc,
               size_t *size) {

    CardinalBootInfo* bootInfo = GetBootInfo();
    if(bootInfo->InitrdStartAddress == 0 | bootInfo->InitrdLength == 0)return false;

    *loc = NULL;
    *size = 0;

    TARHeader *file_entry = (TARHeader*)bootInfo->InitrdStartAddress;
    uint32_t file_param_len = strnlen(file, 100);

    while(file_entry->filename[0] != 0) {
        uint32_t len = strnlen(file_entry->filename, 100);

        if(strcmp(file_entry->filename, file) == 0) {
            *loc = (void*)((uint64_t)file_entry + 512);
            *size = getsize(file_entry->size);
            break;
        }

        file_entry = (TARHeader*)((uint64_t)file_entry + 512 + getsize(file_entry->size));

        if((uint64_t)file_entry % 512) {
            file_entry = (TARHeader*)((uint64_t)file_entry + (512 - (uint64_t)file_entry % 512));
        }
    }

    if(*loc == NULL)return false;
    return true;
}