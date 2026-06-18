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

typedef struct TARHeader
{
    char filename[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag[1];
} TARHeader;

// In-RAM initrd overlay: files supplied at runtime (over serial, via the
// SysDebug `upload` command) take precedence over the baked initrd. This makes
// any boot file -- a module .celf, devices.txt, a boot script -- substitutable
// for development without reflashing the boot medium. General OS facility, not
// tied to any one module.
#define INITRD_OVERLAY_MAX 16
#define INITRD_OVERLAY_ARENA (2 * 1024 * 1024)
static struct
{
    char name[100];
    uint8_t *data;
    size_t len;
} initrd_overlay[INITRD_OVERLAY_MAX];
static int initrd_overlay_cnt = 0;
static uint8_t initrd_overlay_arena[INITRD_OVERLAY_ARENA];
static size_t initrd_overlay_used = 0;

// Copy `data` into the overlay so it shadows initrd file `name` (exact match,
// e.g. "./rtl8169.celf"). A repeated name shadows the previous one (latest
// wins). Returns 0 on success, -1 if the overlay table or arena is full.
int Initrd_AddOverlay(const char *name, const void *data, size_t len)
{
    if (initrd_overlay_cnt >= INITRD_OVERLAY_MAX ||
        initrd_overlay_used + len > INITRD_OVERLAY_ARENA)
        return -1;

    uint8_t *dst = initrd_overlay_arena + initrd_overlay_used;
    memcpy(dst, data, len);
    initrd_overlay_used += len;

    strncpy(initrd_overlay[initrd_overlay_cnt].name, name, 99);
    initrd_overlay[initrd_overlay_cnt].name[99] = 0;
    initrd_overlay[initrd_overlay_cnt].data = dst;
    initrd_overlay[initrd_overlay_cnt].len = len;
    initrd_overlay_cnt++;
    return 0;
}

unsigned int getsize(const char *in)
{

    unsigned int size = 0;
    unsigned int j;
    unsigned int count = 1;

    for (j = 11; j > 0; j--, count *= 8)
        size += ((in[j - 1] - '0') * count);

    return size;
}

bool Initrd_GetFile(const char *file,
                    void **loc,
                    size_t *size)
{

    CardinalBootInfo *bootInfo = GetBootInfo();
    if ((bootInfo->InitrdStartAddress == 0) || (bootInfo->InitrdLength == 0))
        return false;

    *loc = NULL;
    *size = 0;

    //Serial-supplied overlay shadows the baked initrd (see Initrd_AddOverlay).
    for (int i = 0; i < initrd_overlay_cnt; i++)
    {
        if (strcmp(initrd_overlay[i].name, file) == 0)
        {
            *loc = initrd_overlay[i].data;
            *size = initrd_overlay[i].len;
            return true;
        }
    }

    TARHeader *file_entry = (TARHeader *)bootInfo->InitrdStartAddress;
    uint32_t file_param_len = strnlen(file, 100);

    while (file_entry->filename[0] != 0)
    {
        uint32_t len = strnlen(file_entry->filename, 100);

        if (strcmp(file_entry->filename, file) == 0)
        {
            *loc = (void *)((uint64_t)file_entry + 512);
            *size = getsize(file_entry->size);
            break;
        }

        file_entry = (TARHeader *)((uint64_t)file_entry + 512 + getsize(file_entry->size));

        if ((uint64_t)file_entry % 512)
        {
            file_entry = (TARHeader *)((uint64_t)file_entry + (512 - (uint64_t)file_entry % 512));
        }
    }

    if (*loc == NULL)
        return false;
    return true;
}