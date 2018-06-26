/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "elf.h"
#include "boot_information.h"
#include "symbol_db.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>

static uintptr_t loaded_modules[512];
static int loaded_modules_idx = 0;

static int Elf64_GetSymbolValue(Elf64_Ehdr *hdr NONNULL,
                                Elf64_Shdr *shdr NONNULL, int symbol,
                                uint64_t *ret NONNULL) {
    if (symbol == SHN_UNDEF) {
        *ret = 0;
        return 0;
    }

    Elf64_Shdr *shdr_root = (Elf64_Shdr *)((uint8_t *)hdr + hdr->e_shoff);
    Elf64_Shdr *symtab = shdr;
    Elf64_Shdr *strtab = &shdr_root[symtab->sh_link];

    if ((uint64_t)symbol > symtab->sh_size / symtab->sh_entsize)
        PANIC("Symbol out of range!");

    Elf64_Sym *sym = (Elf64_Sym *)(symtab->sh_addr + sizeof(Elf64_Sym) * symbol);

    if (sym->st_shndx == SHN_ABS) {
        *ret = sym->st_value;
        return 0;
    } else if (sym->st_shndx == SHN_UNDEF &&
               ELF64_ST_TYPE(sym->st_info) == STT_NOTYPE) {

        Elf64_Shdr *m_hdr = NULL;
        Elf64_Sym *m_sym = NULL;

        if (symboldb_findmatch(strtab, symtab, sym, &m_hdr, &m_sym) != 0)
            PANIC("Symbol match not found!");

        *ret = m_sym->st_value;
        return 0;

    } else if (ELF64_ST_TYPE(sym->st_info) == STT_FUNC) {
        *ret = sym->st_value;
        return 0;
    } else if (ELF64_ST_TYPE(sym->st_info) == STT_OBJECT) {
        *ret = sym->st_value;
        return 0;
    } else if (ELF64_ST_TYPE(sym->st_info) == STT_SECTION) {
        *ret = sym->st_value;
        return 0;
    } else {
        *ret = (uint64_t)hdr + sym->st_value + shdr_root[sym->st_shndx].sh_offset;
        return 0;
    }

    return -1;
}

static int Elf64_PerformRelocation(Elf64_Ehdr *hdr NONNULL, Elf64_Addr offset,
                                   Elf64_Xword info, Elf64_Sxword addend,
                                   Elf64_Shdr *sym_shdr NONNULL,
                                   Elf64_Shdr *sec_shdr NONNULL) {

    uintptr_t addr = (uintptr_t)hdr + sec_shdr->sh_offset;
    uintptr_t ref_val = addr + offset;
    uint8_t *ref = (uint8_t *)ref_val;

    uint64_t symval = 0;
    if (ELF64_R_SYM(info) != SHN_UNDEF) {
        // get the symbol value

        if (Elf64_GetSymbolValue(hdr, sym_shdr, ELF64_R_SYM(info), &symval))
            PANIC("Failed to retrieve symbol value.");
    }
    switch (ELF64_R_TYPE(info)) {
    case R_AMD64_NONE:
        break;
    case R_AMD64_32: {
        uint32_t result = (uint32_t)symval + (uint32_t)addend;
        memcpy(ref, &result, sizeof(result));
    }
    break;
    case R_AMD64_32S: {
        int32_t result = (int32_t)symval + (int32_t)addend;
        memcpy(ref, &result, sizeof(result));
    }
    break;
    case R_AMD64_64: {
        uint64_t result = symval + addend;
        memcpy(ref, &result, sizeof(result));
    }
    break;

    case R_AMD64_PC32: {
        uint32_t result = (uint32_t)(symval + addend - ref_val);
        memcpy(ref, &result, sizeof(result));
    }
    break;
    case R_AMD64_PC64: {
        uint64_t result = (symval + addend - ref_val);
        memcpy(ref, &result, sizeof(result));
    }
    break;

    case R_AMD64_PLT32: {
        uint32_t result = (symval + addend - ref_val);
        memcpy(ref, &result, sizeof(result));
    }
    break;
    case R_AMD64_GOTPCREL: {
        uint32_t result = (symval + addend - ref_val);
        memcpy(ref, &result, sizeof(result));
        if (*(ref - 2) == 0x8b)
            *(ref - 2) = 0x8d; // Convert to LEA instruction
        else
            PANIC("Expected MOV instruction!");

    }
    break;
    default:
        PANIC("Unsupported Relocation Type.");
        break;
    }

    return 0;
}

int elf_installkernelsymbols() {
    // Obtain the function symbols from the kernel and add their offsets to the
    // table of resolvable functions

    // Update this table with every loaded module, ignore the 'module_init'
    // functions, make those be the entry point returned by elf64Load
    CardinalBootInfo *bInfo = GetBootInfo();
    Elf64_Shdr *shdr_cp = malloc(sizeof(Elf64_Shdr) * bInfo->elf_shdr_num);
    memcpy(shdr_cp, (uint8_t *)bInfo->elf_shdr_addr,
           bInfo->elf_shdr_num * sizeof(Elf64_Shdr));

    //TODO: Verify that this is correct behavior on other machine
    for (uint32_t i = 0; i < bInfo->elf_shdr_num; i++) {
        shdr_cp[i].sh_addr += 0xffffffff80000000;

        if(shdr_cp[i].sh_type == SHT_SYMTAB | shdr_cp[i].sh_type == SHT_STRTAB) {
            //Copy all shdr's into local memory too, and update addresses appropriately
            void *cur_shdr_content = malloc(shdr_cp[i].sh_size);
            memcpy(cur_shdr_content, (void*)shdr_cp[i].sh_addr, shdr_cp[i].sh_size);
            shdr_cp[i].sh_addr = (uint64_t)cur_shdr_content;
        }
    }



    for (uint32_t i = 0; i < bInfo->elf_shdr_num; i++) {
        if (shdr_cp[i].sh_type == SHT_SYMTAB) {
            Elf64_Sym *sym = (Elf64_Sym *)shdr_cp[i].sh_addr;

            for (uint32_t j = 0; j < shdr_cp[i].sh_size / shdr_cp[i].sh_entsize;
                    j++) {
                if (ELF64_ST_TYPE(sym[j].st_info) == STT_FUNC) {
                    Elf64_Shdr *strtab = &shdr_cp[shdr_cp[i].sh_link];
                    symboldb_add(strtab, &shdr_cp[i], &sym[j]);
                }
            }
        }
    }

    return 0;
}

void *elf_resolvefunction(const char *name) {

    if (name == NULL)
        return NULL;

    Elf64_Shdr *m_hdr = NULL;
    Elf64_Sym *m_sym = NULL;

    if (symboldb_findfunc(name, &m_hdr, &m_sym) == 0) {
        return (void *)m_sym->st_value;
    }

    return NULL;
}

int elf_load(void *elf, size_t elf_len, int (**entry_point)()) {

    if (elf == NULL)
        return -1;

    if (entry_point == NULL)
        return -1;

    if (elf_len == 0)
        return -1;

    bool reloading = false;

    Elf64_Ehdr *hdr = elf;
    for(int i = 0; i < loaded_modules_idx; i++)
        if(loaded_modules[i] == (uintptr_t)elf){
            reloading = true;
            break;
        }

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
    if (c_hdr->e_type != ET_REL)
        return -2;
    if (c_hdr->e_machine == ET_NONE)
        return -2;
    if (c_hdr->e_version != EV_CURRENT)
        return -2;
    if (c_hdr->e_ident[EI_OSABI] != ELFOSABI_GNU &&
            c_hdr->e_ident[EI_OSABI] != ELFOSABI_NONE)
        return -2;

    Elf64_Shdr *shdr_root = (Elf64_Shdr *)((uint8_t *)hdr + hdr->e_shoff);
    for (int i = 0; i < hdr->e_shnum; i++) {
        Elf64_Shdr *shdr = &shdr_root[i];

        if ((shdr->sh_type == SHT_STRTAB) | (shdr->sh_type == SHT_PROGBITS))
            shdr->sh_addr = (uintptr_t)hdr + shdr->sh_offset;
    }

    if(reloading) {
        for (int i = 0; i < hdr->e_shnum; i++) {
            Elf64_Shdr *shdr = &shdr_root[i];
            if (shdr->sh_type == SHT_SYMTAB) {

                Elf64_Sym *sym = (Elf64_Sym *)((uint8_t *)hdr + shdr->sh_offset);
                
                for (uint32_t j = 0; j < shdr->sh_size / shdr->sh_entsize; j++) {

                    if ((sym[j].st_shndx == SHN_ABS) | (sym[j].st_shndx == SHN_UNDEF))
                        continue;

                    Elf64_Shdr *sec_shdr = &shdr_root[sym[j].st_shndx];

                    if (ELF64_ST_TYPE(sym[j].st_info) == STT_FUNC) {
                        Elf64_Shdr *strtab = &shdr_root[shdr->sh_link];
                        
                        if (strcmp((char *)strtab->sh_addr + sym[j].st_name, "module_init") ==
                                0) {
                            *entry_point = (int (*)())sym[j].st_value;
                        }
                    }
                }
            }
        }
        return 0;
    }

    // allocate SHT_NOBITS and SHF_ALLOC sections
    for (int i = 0; i < hdr->e_shnum; i++) {
        Elf64_Shdr *shdr = &shdr_root[i];
        if (shdr->sh_type == SHT_NOBITS) {

            if (shdr->sh_size == 0)
                continue;

            if (shdr->sh_flags & SHF_ALLOC) {
                // Allocate and clear memory
                void *mem = malloc(shdr->sh_size);
                if (mem == NULL)
                    PANIC("Critical Memory Allocation Failure!");

                memset(mem, 0, shdr->sh_size);
                shdr->sh_offset = (uint64_t)mem - (uint64_t)hdr;
                shdr->sh_addr = (uint64_t)mem;
            }

        } else if (shdr->sh_type == SHT_SYMTAB) {

            Elf64_Sym *sym = (Elf64_Sym *)((uint8_t *)hdr + shdr->sh_offset);
            shdr->sh_addr = (uintptr_t)hdr + shdr->sh_offset;

            for (uint32_t j = 0; j < shdr->sh_size / shdr->sh_entsize; j++) {

                if ((sym[j].st_shndx == SHN_ABS) | (sym[j].st_shndx == SHN_UNDEF))
                    continue;

                Elf64_Shdr *sec_shdr = &shdr_root[sym[j].st_shndx];

                if (ELF64_ST_TYPE(sym[j].st_info) == STT_FUNC) {
                    Elf64_Shdr *strtab = &shdr_root[shdr->sh_link];
                    sym[j].st_value += sec_shdr->sh_addr;

                    if (strcmp((char *)strtab->sh_addr + sym[j].st_name, "module_init") ==
                            0) {
                        *entry_point = (int (*)())sym[j].st_value;
                    } else if (ELF64_ST_VISIBILITY(sym[j].st_other) !=
                               STV_HIDDEN && ELF64_ST_BIND(sym[j].st_info) != STB_LOCAL) // Don't add hidden symbols
                        symboldb_add(strtab, shdr, &sym[j]);
                } else if (ELF64_ST_TYPE(sym[j].st_info) == STT_SECTION) {
                    sym[j].st_value += sec_shdr->sh_addr;
                } else if (ELF64_ST_TYPE(sym[j].st_info) == STT_OBJECT) {
                    sym[j].st_value += sec_shdr->sh_addr;
                }
            }
        }
    }

    // perform relocations for remaining symbols
    for (uint64_t i = 0; i < hdr->e_shnum; i++) {
        Elf64_Shdr *shdr = &shdr_root[i];

        if (shdr->sh_type == SHT_REL) {
            Elf64_Rel *rel = (Elf64_Rel *)((uint8_t *)hdr + shdr->sh_offset);

            for (uint64_t j = 0; j < shdr->sh_size / shdr->sh_entsize; j++) {

                Elf64_Shdr *target_shdr = &shdr_root[shdr->sh_link];
                Elf64_Shdr *dest_shdr = &shdr_root[shdr->sh_info];

                // Do relocation
                if (Elf64_PerformRelocation(hdr, rel[j].r_offset, rel[j].r_info, 0,
                                            target_shdr, dest_shdr))
                    PANIC("Failed Rel relocation!");
            }
        } else if (shdr->sh_type == SHT_RELA) {
            Elf64_Rela *rel = (Elf64_Rela *)((uint8_t *)hdr + shdr->sh_offset);

            for (uint64_t j = 0; j < shdr->sh_size / shdr->sh_entsize; j++) {

                Elf64_Shdr *sym_shdr = &shdr_root[shdr->sh_link];
                Elf64_Shdr *sec_shdr = &shdr_root[shdr->sh_info];

                // Do relocation
                if (Elf64_PerformRelocation(hdr, rel[j].r_offset, rel[j].r_info,
                                            rel[j].r_addend, sym_shdr, sec_shdr))
                    PANIC("Failed Rela relocation!");
            }
        }
    }

    loaded_modules[loaded_modules_idx++] = (uintptr_t)elf;

    return 0;
}
