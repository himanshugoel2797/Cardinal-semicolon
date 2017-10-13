/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "elf.h"
#include "bootstrap_alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// First allocate SHT_NOBITS and SHF_ALLOC sections

// Then load the remaining sections, applying relocations

static int Elf64_GetSymbolValue(Elf64_Ehdr *hdr, int table, int symbol, uint64_t *ret) {
    if (table == SHN_UNDEF | symbol == SHN_UNDEF){
        *ret = 0;
        return 0;
    }

    Elf64_Shdr *shdr_root = (Elf64_Shdr*)((uint8_t*)hdr + hdr->e_shoff);
    Elf64_Shdr *symtab = &shdr_root[table];

    if((uint64_t)symbol > symtab->sh_size / symtab->sh_entsize)
        PANIC("Symbol out of range!");

    Elf64_Sym *sym = (Elf64_Sym*) (hdr + symtab->sh_offset + sizeof(Elf64_Sym) * symbol);
    if(sym->st_shndx == SHN_ABS){
        *ret = sym->st_value;
        return 0;
    }else{
        *ret = (uint64_t)hdr + sym->st_value + shdr_root[sym->st_shndx].sh_offset;
        return 0;
    }


    return -1;
}

static int Elf64_PerformRelocation(Elf64_Ehdr *hdr, Elf64_Addr offset, Elf64_Xword info, Elf64_Sxword addend, Elf64_Shdr *shdr) {    

    if (hdr == NULL)
        return -1;

    if (shdr == NULL)
        return -1;

    uint8_t *addr = (uint8_t*)hdr + shdr->sh_offset;
    uint8_t *ref = addr + offset;

    uint64_t symval = 0;
    if(ELF64_R_SYM(info) != SHN_UNDEF) {
        //get the symbol value
        if(Elf64_GetSymbolValue(hdr, shdr->sh_link, ELF64_R_SYM(info), &symval))
            PANIC("Failed to retrieve symbol value.");
    }
    switch(ELF64_R_TYPE(info)) {
        case R_AMD64_NONE:
        break;
        case R_AMD64_32:
            DEBUG_ECHO("R_AMD64_32");
            *(uint32_t*)ref = (uint32_t)(symval + *(uint32_t*)ref + addend);
        break;
        case R_AMD64_32S:
            *(int32_t*)ref = (int32_t)((int64_t)symval + *(int32_t*)ref + (int64_t)addend);
        break;
        case R_AMD64_64:
            *(uint64_t*)ref = symval + *(uint64_t*)ref + addend;
        break;

        case R_AMD64_PC32:
            *(int32_t*)ref =   (int32_t)((int64_t)symval + *(int32_t*)ref + (int64_t)addend - (int64_t)ref); 
        break;
        case R_AMD64_PC64:
            *(int64_t*)ref =   (int64_t)((int64_t)symval + *(int64_t*)ref + (int64_t)addend - (int64_t)ref); 
        break;

        default:
            PANIC("Unsupported Relocation Type.");
        break;
    }

    return 0;
}

int Elf64Load(void *elf, size_t elf_len) {

  if(elf == NULL)
    return -1;

  if(elf_len == 0)
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

  if (c_hdr->e_ident[EI_DATA] != ELFDATA2MSB)
    return -2;

  if (c_hdr->e_type == ET_NONE)
    return -2;

  if (c_hdr->e_machine == ET_NONE)
    return -2;

  if (c_hdr->e_version != EV_CURRENT)
    return -2;

  if (c_hdr->e_ident[EI_OSABI] != ELFOSABI_GNU &&
      c_hdr->e_ident[EI_OSABI] != ELFOSABI_NONE)
    return -2;

  // allocate SHT_NOBITS and SHF_ALLOC sections
  Elf64_Shdr *shdr_root = (Elf64_Shdr *)((uint8_t *)hdr + hdr->e_shoff);
  for (int i = 0; i < hdr->e_shnum; i++) {
    Elf64_Shdr *shdr = &shdr_root[i];

    if (shdr->sh_type == SHT_NOBITS) {
      if (shdr->sh_size == 0)
        continue;

      if (shdr->sh_flags & SHF_ALLOC) {
        // Allocate and clear memory
        void *mem = bootstrap_malloc(shdr->sh_size);
        if (mem == NULL)
          PANIC("Critical Memory Allocation Failure!");

        memset(mem, 0, shdr->sh_size);
        shdr->sh_offset = (uint64_t)mem - (uint64_t)hdr;
      }
    }
  }

  //perform relocations for remaining symbols
  for(uint64_t i = 0; i < hdr->e_shnum; i++) {
      Elf64_Shdr *shdr = &shdr_root[i];

      void *rel_root = (void*)((uint8_t*)hdr + shdr->sh_offset);
      if (shdr->sh_type == SHT_REL) {
        for(uint64_t j = 0; j < shdr->sh_size / shdr->sh_entsize; j++) {
            if(shdr->sh_entsize == sizeof(Elf64_Rel)) {
                Elf64_Rel *rel = (Elf64_Rel*)rel_root + j;

                //Do relocation
                if(Elf64_PerformRelocation(hdr, rel->r_offset, rel->r_info, 0, shdr))
                PANIC("Failed Rel relocation!");

            }else
                PANIC("Unknown Relocation Type!");
        }
      }else if(shdr->sh_type == SHT_RELA) {
        for(uint64_t j = 0; j < shdr->sh_size / shdr->sh_entsize; j++) {
            if(shdr->sh_entsize == sizeof(Elf64_Rela)){
                Elf64_Rela *rel = (Elf64_Rela*)rel_root + j;

                //Do relocation
                if(Elf64_PerformRelocation(hdr, rel->r_offset, rel->r_info, rel->r_addend, shdr))
                    PANIC("Failed Rela relocation!");
            }else
                PANIC("Unknown Relocation Type!");
        }
      }
  }

  return 0;
}
