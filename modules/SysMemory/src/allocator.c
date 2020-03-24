/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "SysVirtualMemory/vmem.h"
#include "SysPhysicalMemory/phys_mem.h"

typedef struct mem_node
{
    struct mem_node *next;
    uint32_t len;
    bool isFree;
    uint8_t data[0];
} mem_node_t;

static mem_node_t *root = NULL;
static _Atomic uint32_t node_cnt = 0;

void *WEAK malloc(size_t sz)
{
    if (sz == 0)
        return NULL;

    int cli_state = cli();
    //Align size with 16 bytes, so all allocations are 16-byte aligned
    if (sz % 8)
        sz += 8 - (sz % 8);

    //Search list for best fitting free region
    mem_node_t *cur_best_fit = NULL;
    mem_node_t *iter = root;
    while (iter != NULL)
    {
        if (iter->isFree && iter->len >= sz)
        {
            if (cur_best_fit != NULL)
            {
                if (iter->len < cur_best_fit->len)
                    cur_best_fit = iter;
            }
            else
                cur_best_fit = iter;
        }
        iter = iter->next;
    }

    //If none found, allocate a new set of pages
    if (cur_best_fit == NULL)
    {

        //allocate new page set
        size_t alloc_sz = sz + sizeof(mem_node_t);
        if (alloc_sz % KiB(4))
            alloc_sz += KiB(4) - (alloc_sz % KiB(4));

        uintptr_t phys = pagealloc_alloc(0, 0, physmem_alloc_flags_data, alloc_sz);

        //setup a mem_node_t at the top
        mem_node_t *n_node = (mem_node_t *)vmem_phystovirt(phys, alloc_sz, vmem_flags_cachewriteback | vmem_flags_kernel | vmem_flags_rw);
        n_node->next = NULL;
        n_node->len = alloc_sz - sizeof(mem_node_t);
        n_node->isFree = false;

        cur_best_fit = n_node;
        node_cnt++;

        //insert the node into the list
        if (root == NULL)
        {
            root = n_node;
        }
        else
        {
            n_node->next = root->next;
            root->next = n_node;
        }
    }

    if (cur_best_fit->len > sz + sizeof(mem_node_t))
    {

        //Create a new free node with the remaining memory
        mem_node_t *n_node = (mem_node_t *)(cur_best_fit->data + sz);
        n_node->next = NULL;
        n_node->len = cur_best_fit->len - sz - sizeof(mem_node_t);
        n_node->isFree = true;

        node_cnt++;

        if (n_node->len <= sz / 2)
        {
            //TODO: report freenode creations that are at or below half the allocation size, this can be used to tune the best 'fit' scoring
        }

        //insert the node into the list
        n_node->next = cur_best_fit->next;
        cur_best_fit->next = n_node;

        //shrink the best fit node to fully fit
        cur_best_fit->len = sz;
    }

    cur_best_fit->isFree = false;
    void *retAddr = cur_best_fit->data;
    sti(cli_state);

    return retAddr;
}

static void mem_compact()
{
    //TODO: compact list if it gets too big, to clear out small fragmented areas
    //TODO: detect adjacent free areas and merge them
}

static void *doublefree_addr = NULL;
void print_free_addr()
{
    char tmp[10];
    DEBUG_PRINT("At ");
    DEBUG_PRINT(itoa((uint64_t)doublefree_addr >> 32, tmp, 16));
    DEBUG_PRINT(itoa((uint64_t)doublefree_addr, tmp, 16));
    DEBUG_PRINT("\r\n");
}

void WEAK free(void *sz)
{

    if (sz == NULL)
        return;

    int cli_state = cli();

    //access the node info
    mem_node_t *desc = (mem_node_t *)((intptr_t)sz - sizeof(mem_node_t));

    //Remove any freed pages from the allocator

    //Mark the remaining space as available
    if (desc->isFree)
    {
        doublefree_addr = sz;
        PANIC("Double free detected.");
    }

    desc->isFree = true;

    //TODO: implement returning remaining memory to system
    if (node_cnt >= 512)
        mem_compact();

    sti(cli_state);
}