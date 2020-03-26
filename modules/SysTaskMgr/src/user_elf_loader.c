/**
 * Copyright (c) 2020 Himanshu Goel
 * 
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <cardinal/local_spinlock.h>

#include "SysVirtualMemory/vmem.h"

#include "task_priv.h"
#include "error.h"
#include "elf.h"

int user_elf_load(cs_id task_id, void *elf, size_t elf_len, void (**entry_point)(void *))
{
    if (elf == NULL)
        return -1;

    if (entry_point == NULL)
        return -1;

    if (elf_len == 0)
        return -1;

    Elf64_Ehdr *hdr = elf;

    // Verify the header
    Elf_CommonEhdr *c_hdr = &hdr->e_hdr;
    if (c_hdr->e_ident[EI_MAG0] != ELF_MAG0)
        return -1;
    if (c_hdr->e_ident[EI_MAG1] != ELF_MAG1)
        return -1;
    if (c_hdr->e_ident[EI_MAG2] != ELF_MAG2)
        return -1;
    if (c_hdr->e_ident[EI_MAG3] != ELF_MAG3)
        return -1;

    if (c_hdr->e_ident[EI_DATA] != ELFDATA2LSB)
        return -2;
    if (c_hdr->e_type != ET_EXEC)
        return -2;
    if (c_hdr->e_machine == ET_NONE)
        return -2;
    if (c_hdr->e_version != EV_CURRENT)
        return -2;
    if (c_hdr->e_ident[EI_OSABI] != ELFOSABI_GNU &&
        c_hdr->e_ident[EI_OSABI] != ELFOSABI_NONE)
        return -2;

    *entry_point = (void (*)(void *))hdr->e_entry;

    Elf64_Shdr *
        shdr_root = (Elf64_Shdr *)((uint8_t *)hdr + hdr->e_shoff);

    // setup sections into target process
    for (int i = 0; i < hdr->e_shnum; i++)
    {
        Elf64_Shdr *shdr = &shdr_root[i];
        if (shdr->sh_type == SHT_NOBITS)
        {
            if (shdr->sh_size == 0)
                continue;

            if (shdr->sh_flags & SHF_ALLOC)
            {
                // Round down target address to page boundary
                uint64_t dst_addr = shdr->sh_addr & ~(KiB(4) - 1);
                uint64_t dst_sz = shdr->sh_size;

                // Round up page size
                if (dst_sz % KiB(4) != 0)
                    dst_sz += KiB(4) - (dst_sz % KiB(4));

                // Determine if mapping for each page exists
                // Create mapping for missing pages
                intptr_t phys_dst_addr = 0;
                cs_id dst_shmem_id = 0;
                for (uint64_t j = 0; j < dst_sz; j += KiB(4))
                    if (task_virttophys(task_id, (intptr_t)dst_addr + j, &phys_dst_addr) != CS_OK)
                    {
                        task_map(task_id, NULL, (intptr_t)(dst_addr + j), KiB(4), task_map_none, task_map_perm_cachewriteback | task_map_perm_writeonly, 0, 0, &dst_shmem_id);
                        if (task_virttophys(task_id, (intptr_t)dst_addr + j, &phys_dst_addr) != CS_OK)
                            PANIC("[SysTaskMgr] Elf memory allocation failure.");

                        //task_map ensures a clear page
                    }
            }
        }
        else if (shdr->sh_type == SHT_SYMTAB)
        {
            Elf64_Sym *sym = (Elf64_Sym *)((uint8_t *)hdr + shdr->sh_offset);
            shdr->sh_addr = (uintptr_t)hdr + shdr->sh_offset;

            for (uint32_t j = 0; j < shdr->sh_size / shdr->sh_entsize; j++)
            {

                if ((sym[j].st_shndx == SHN_ABS) | (sym[j].st_shndx == SHN_UNDEF))
                    continue;

                Elf64_Shdr *sec_shdr = &shdr_root[sym[j].st_shndx];

                if (ELF64_ST_TYPE(sym[j].st_info) == STT_FUNC)
                {
                    Elf64_Shdr *strtab = &shdr_root[shdr->sh_link];
                    sym[j].st_value += sec_shdr->sh_addr;
                }
                else if (ELF64_ST_TYPE(sym[j].st_info) == STT_SECTION)
                {
                    sym[j].st_value += sec_shdr->sh_addr;
                }
                else if (ELF64_ST_TYPE(sym[j].st_info) == STT_OBJECT)
                {
                    sym[j].st_value += sec_shdr->sh_addr;
                }
            }
        }
        else if (shdr->sh_flags & SHF_ALLOC)
        {
            // Round down target address to page boundary
            uint64_t dst_addr = shdr->sh_addr & ~(KiB(4) - 1);
            uint64_t dst_sz = shdr->sh_size;

            uint64_t targ_addr = shdr->sh_addr;
            uint64_t src_off = shdr->sh_addr & (KiB(4) - 1);
            uint64_t src_addr = (uint64_t)hdr + shdr->sh_offset;
            uint64_t src_sz = shdr->sh_size;

            // Round up page size
            if (dst_sz % KiB(4) != 0)
                dst_sz += KiB(4) - (dst_sz % KiB(4));

            // Determine if mapping for each page exists
            // Create mapping for missing pages
            intptr_t phys_dst_addr = 0;
            cs_id dst_shmem_id = 0;
            for (uint64_t j = 0; j < dst_sz; j += KiB(4))
            {
                if (task_virttophys(task_id, (intptr_t)dst_addr + j, &phys_dst_addr) != CS_OK)
                    task_map(task_id, NULL, (intptr_t)(dst_addr + j), KiB(4), task_map_none, task_map_perm_cachewriteback | task_map_perm_execute | /*(shdr->sh_flags & SHF_EXECINSTR ? task_map_perm_execute :*/ (task_map_perm_writeonly), 0, 0, &dst_shmem_id);

                if (task_virttophys(task_id, (intptr_t)dst_addr + j, &phys_dst_addr) != CS_OK)
                    PANIC("[SysTaskMgr] Elf memory allocation failure.");

                //task_map ensures a clear page
                intptr_t map_vaddr = vmem_phystovirt(phys_dst_addr, KiB(4), task_map_perm_cachewriteback);

                uint64_t dst_off = targ_addr - (dst_addr + j);
                uint64_t cp_sz = MIN(KiB(4) - dst_off, src_sz);

                memcpy((uint64_t *)(map_vaddr + dst_off), (uint64_t *)src_addr, cp_sz);

                src_addr += cp_sz;
                targ_addr += cp_sz;
                src_sz -= cp_sz;
            }
        }
    }

    return 0;
}