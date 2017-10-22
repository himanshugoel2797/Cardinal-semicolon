/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "elf.h"
#include "boot_information.h"
#include "bootstrap_alloc.h"
#include "symbol_db.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// First allocate SHT_NOBITS and SHF_ALLOC sections

// Then load the remaining sections, applying relocations

static int Elf64_GetSymbolValue(Elf64_Ehdr *hdr, Elf64_Shdr *shdr, int symbol,
                                uint64_t *ret) {
  if (symbol == SHN_UNDEF) {
    *ret = 0;
    return 0;
  }

  Elf64_Shdr *shdr_root = (Elf64_Shdr *)((uint8_t *)hdr + hdr->e_shoff);
  Elf64_Shdr *symtab = shdr;
  Elf64_Shdr *strtab = &shdr_root[symtab->sh_link];

  if ((uint64_t)symbol > symtab->sh_size / symtab->sh_entsize)
    PANIC("Symbol out of range!");

  Elf64_Sym *sym =
      (Elf64_Sym *)(symtab->sh_addr + sizeof(Elf64_Sym) * symbol);

  if (sym->st_shndx == SHN_ABS) {
    *ret = sym->st_value;
    return 0;
  } else if (sym->st_shndx == SHN_UNDEF && ELF64_ST_TYPE(sym->st_info) == STT_NOTYPE) {
    
    Elf64_Shdr *m_hdr = NULL;
    Elf64_Sym *m_sym = NULL;
    
    if (symboldb_findmatch(strtab, symtab, sym, &m_hdr, &m_sym) != 0)
      PANIC("Symbol match not found!");

    *ret = m_sym->st_value;
    return 0;

  } else {
    *ret = (uint64_t)hdr + sym->st_value + shdr_root[sym->st_shndx].sh_offset;
    return 0;
  }

  return -1;
}

static int Elf64_PerformRelocation(Elf64_Ehdr *hdr, Elf64_Addr offset,
                                   Elf64_Xword info, Elf64_Sxword addend,
                                   Elf64_Shdr *sym_shdr, Elf64_Shdr *sec_shdr) {

  if (hdr == NULL)
    return -1;

  if (sym_shdr == NULL)
    return -1;

  if (sec_shdr == NULL)
  return -1;

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
  } break;
  case R_AMD64_32S: {
    int32_t result = (int32_t)symval + (int32_t)addend;
    memcpy(ref, &result, sizeof(result));
  } break;
  case R_AMD64_64: {
    uint64_t result = symval + addend;
    memcpy(ref, &result, sizeof(result));
  } break;

  case R_AMD64_PC32: {
    uint32_t result = (uint32_t)(symval + addend - ref_val);
    memcpy(ref, &result, sizeof(result));
  } break;
  case R_AMD64_PC64: {
    uint64_t result = (symval + addend - ref_val);
    memcpy(ref, &result, sizeof(result));
  } break;

  case R_AMD64_PLT32: {
    uint32_t result = (symval + addend - ref_val);
    memcpy(ref, &result, sizeof(result));
  } break;
  default:
    PANIC("Unsupported Relocation Type.");
    break;
  }

  return 0;
}

int Elf64InstallKernelSymbols() {
  // Obtain the function symbols from the kernel and add their offsets to the
  // table of resolvable functions

  // Update this table with every loaded module, ignore the 'module_init'
  // functions, make those be the entry point returned by elf64Load
  CardinalBootInfo *bInfo = GetBootInfo();
  Elf64_Shdr *shdr_cp = bootstrap_malloc(sizeof(Elf64_Shdr) * bInfo->elf_shdr_num);
  memcpy(shdr_cp, (uint8_t*)bInfo->elf_shdr_addr, bInfo->elf_shdr_num * sizeof(Elf64_Shdr));
  
  for (uint32_t i = 0; i < bInfo->elf_shdr_num; i++) {
    
    if (shdr_cp[i].sh_type == SHT_SYMTAB) {
      Elf64_Sym *sym = (Elf64_Sym *)shdr_cp[i].sh_addr;
      
      for (uint32_t j = 0; j < shdr_cp[i].sh_size / shdr_cp[i].sh_entsize; j++) {
        if (ELF64_ST_TYPE(sym[j].st_info) == STT_FUNC) {
          Elf64_Shdr *strtab = &shdr_cp[shdr_cp[i].sh_link];
          symboldb_add(strtab, &shdr_cp[i], &sym[j]);
        }
      }
    }
  }


  return 0;
}

int Elf64Load(void *elf, size_t elf_len, uintptr_t *entry_pt) {

  if (elf == NULL)
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

  if (c_hdr->e_ident[EI_DATA] == ELFDATA2MSB)
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

    if ((shdr->sh_type == SHT_STRTAB) | (shdr->sh_type == SHT_PROGBITS)) {

      shdr->sh_addr = (uintptr_t)hdr + shdr->sh_offset;
    
    }else if (shdr->sh_type == SHT_NOBITS) {
      
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

    } else if (shdr->sh_type == SHT_SYMTAB) {
      
      Elf64_Shdr *sec_shdr = &shdr_root[shdr->sh_info];
      Elf64_Sym *sym = (Elf64_Sym *)((uint8_t *)hdr + shdr->sh_offset);
      shdr->sh_addr = (uintptr_t)hdr + shdr->sh_offset;

      for (uint32_t j = 0; j < shdr->sh_size / shdr->sh_entsize; j++) {
        if (ELF64_ST_TYPE(sym[j].st_info) == STT_FUNC) {
          Elf64_Shdr *strtab = (Elf64_Shdr *)((uint8_t *)shdr_root + shdr->sh_link * hdr->e_shentsize);
          sym->st_value += sec_shdr->sh_addr;
          symboldb_add(strtab, shdr, sym);
        }
      }

    }
  }

  // perform relocations for remaining symbols
  for (uint64_t i = 0; i < hdr->e_shnum; i++) {
    Elf64_Shdr *shdr = &shdr_root[i];

    void *rel_root = (void *)((uint8_t *)hdr + shdr->sh_offset);
    if (shdr->sh_type == SHT_REL) {

      for (uint64_t j = 0; j < shdr->sh_size / shdr->sh_entsize; j++) {
          
        Elf64_Rel *rel = (Elf64_Rel *)rel_root + j;
        Elf64_Shdr *target_shdr = &shdr_root[shdr->sh_link];
        Elf64_Shdr *dest_shdr = &shdr_root[shdr->sh_info];

        // Do relocation
        if (Elf64_PerformRelocation(hdr, rel->r_offset, rel->r_info, 0, target_shdr, dest_shdr))
          PANIC("Failed Rel relocation!");

      }
    } else if (shdr->sh_type == SHT_RELA) {
      
      for (uint64_t j = 0; j < shdr->sh_size / shdr->sh_entsize; j++) {

        Elf64_Rela *rel = (Elf64_Rela *)rel_root + j;
        Elf64_Shdr *sym_shdr = &shdr_root[shdr->sh_link];
        Elf64_Shdr *sec_shdr = &shdr_root[shdr->sh_info];

        // Do relocation
        if (Elf64_PerformRelocation(hdr, rel->r_offset, rel->r_info, rel->r_addend, sym_shdr, sec_shdr))
           PANIC("Failed Rela relocation!");

      }
    }
  }

  *entry_pt = shdr_root[1].sh_addr + 0xe0;
  return 0;
}
