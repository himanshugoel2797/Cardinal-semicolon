#include "symbol_db.h"
#include <stdlib.h>
#include <string.h>
#include <types.h>

#define MAX_SYMBOL_CNT (65537)

#define TO_SYM(x) ((Elf64_Sym *)(symbol_e_offsets[x]))
#define TO_HDR(x) ((Elf64_Shdr *)(symbol_hdr_offsets[x]))
#define TO_STRTAB(x) ((Elf64_Shdr *)(symbol_strhdr_offsets[x]))

#define TO_OFF(x) ((uintptr_t)x)

static uint64_t *symbol_e_offsets = NULL;
static uint64_t *symbol_hdr_offsets = NULL;
static uint64_t *symbol_strhdr_offsets = NULL;
static uint32_t symbol_cnt = 0;

void symboldb_init() {
  symbol_e_offsets = malloc(MAX_SYMBOL_CNT * sizeof(uint64_t));
  symbol_hdr_offsets = malloc(MAX_SYMBOL_CNT * sizeof(uint64_t));
  symbol_strhdr_offsets = malloc(MAX_SYMBOL_CNT * sizeof(uint64_t));

  memset(symbol_e_offsets, 0, MAX_SYMBOL_CNT * sizeof(uint64_t));
  memset(symbol_hdr_offsets, 0, MAX_SYMBOL_CNT * sizeof(uint64_t));
  memset(symbol_strhdr_offsets, 0, MAX_SYMBOL_CNT * sizeof(uint64_t));
}

#define FNV1A_BASIS 2166136261
#define FNV1A_PRIME 16777619
static uint32_t hash(const char *src, size_t src_len) {
  uint32_t hash = FNV1A_BASIS;
  for (size_t i = 0; i < src_len; i++) {
    hash ^= src[i];
    hash *= FNV1A_PRIME;
  }
  return hash;
}

static uint32_t murmur3_32(const char *key, size_t len, uint32_t seed) {
  uint32_t h = seed;
  if (len > 3) {
    const uint32_t *key_x4 = (const uint32_t *)key;
    size_t i = len >> 2;
    do {
      uint32_t k = *key_x4++;
      k *= 0xcc9e2d51;
      k = (k << 15) | (k >> 17);
      k *= 0x1b873593;
      h ^= k;
      h = (h << 13) | (h >> 19);
      h = (h * 5) + 0xe6546b64;
    } while (--i);
    key = (const char *)key_x4;
  }
  if (len & 3) {
    size_t i = len & 3;
    uint32_t k = 0;
    key = &key[i - 1];
    do {
      k <<= 8;
      k |= *key--;
    } while (--i);
    k *= 0xcc9e2d51;
    k = (k << 15) | (k >> 17);
    k *= 0x1b873593;
    h ^= k;
  }
  h ^= len;
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;
  return h;
}

static int symboldb_addentry(uint32_t idx, Elf64_Shdr *strhdr, Elf64_Shdr *hdr,
                             Elf64_Sym *sym) {
  if (symbol_e_offsets[idx] == 0) {
    // insert the entry
    symbol_e_offsets[idx] = TO_OFF(sym);
    symbol_hdr_offsets[idx] = TO_OFF(hdr);
    symbol_strhdr_offsets[idx] = TO_OFF(strhdr);
    return 0;
  } else {
    // Check if the entry is the same name and weak
    Elf64_Shdr *n_hdr = TO_HDR(idx);
    Elf64_Shdr *n_strtab = TO_STRTAB(idx);
    Elf64_Sym *n_sym = TO_SYM(idx);

    char *orig_sym_str = (char *)strhdr->sh_addr + sym->st_name;
    char *ent_sym_str = (char *)n_strtab->sh_addr + n_sym->st_name;

    if (strcmp(orig_sym_str, ent_sym_str) != 0)
      return -1;

    // check if symbol is weak and replace if so
    // TODO: maybe also override the previous entry with a jump to the new one
    if (ELF64_ST_BIND(n_sym->st_info) == STB_WEAK) {
      symbol_e_offsets[idx] = TO_OFF(sym);
      symbol_hdr_offsets[idx] = TO_OFF(hdr);
      symbol_strhdr_offsets[idx] = TO_OFF(strhdr);
      return 0;
    }

    return -1;
  }

  return -1;
}

static int symboldb_entrymatch(uint32_t idx, Elf64_Shdr *strhdr,
                               Elf64_Shdr *hdr, Elf64_Sym *sym,
                               Elf64_Shdr **r_hdr, Elf64_Sym **r_sym) {

  if (symbol_e_offsets[idx] == 0)
    return -1;

  hdr = NULL;

  Elf64_Shdr *n_shdr = TO_HDR(idx);
  Elf64_Shdr *n_strtab = TO_STRTAB(idx);
  Elf64_Sym *n_sym = TO_SYM(idx);
  char *n_str = (char *)strhdr->sh_addr + sym->st_name;
  char *sym_str = (char *)n_strtab->sh_addr + n_sym->st_name;

  if (strcmp(sym_str, n_str) == 0) {
    *r_hdr = n_shdr;
    *r_sym = n_sym;
    return 0;
  }

  return -1;
}

int symboldb_add(Elf64_Shdr *strhdr, Elf64_Shdr *hdr, Elf64_Sym *symbol) {

  // Compute the hash and add it to the table
  // If hash collides, check if the names match and weak symbol, replace it
  // If names dont match, hash the hash and try again
  // If failure, report failure and error out

  if (symbol_cnt == MAX_SYMBOL_CNT)
    PANIC("CRITICAL ERROR: Out of symbol cache space!");

  // Get the symbol name
  char *sym_str = (char *)strhdr->sh_addr + symbol->st_name;

  // hash and modulus table len
  uint32_t s_hash = hash(sym_str, strlen(sym_str));
  uint32_t idx = s_hash % MAX_SYMBOL_CNT;

  if (symboldb_addentry(idx, strhdr, hdr, symbol) == 0) {
    symbol_cnt++;
    return 0;
  }

  s_hash = murmur3_32((char *)&sym_str, strlen(sym_str),
                      sym_str[0] + strlen(sym_str));

  idx = s_hash % MAX_SYMBOL_CNT;

  if (symboldb_addentry(idx, strhdr, hdr, symbol) == 0) {
    symbol_cnt++;
    return 0;
  }

  DEBUG_PRINT(sym_str);
  PANIC("CRITICAL ERROR: Could not store symbol.");
  return 0;
}

int symboldb_findfunc(const char *str, Elf64_Shdr **r_hdr, Elf64_Sym **r_sym) {
  if (str == NULL)
    return -1;

  if (r_hdr == NULL)
    return -1;

  if (r_sym == NULL)
    return -1;

  uint32_t str_hash = hash(str, strlen(str));
  uint32_t idx = str_hash % MAX_SYMBOL_CNT;

  if (symbol_e_offsets[idx] != 0) {
    Elf64_Shdr *n_shdr = TO_HDR(idx);
    Elf64_Shdr *n_strtab = TO_STRTAB(idx);
    Elf64_Sym *n_sym = TO_SYM(idx);
    char *sym_str = (char *)n_strtab->sh_addr + n_sym->st_name;

    if (strcmp(str, sym_str) == 0) {
      *r_hdr = n_shdr;
      *r_sym = n_sym;
      return 0;
    }
  }

  str_hash = murmur3_32((char *)&str, strlen(str), str[0] + strlen(str));
  idx = str_hash % MAX_SYMBOL_CNT;

  if (symbol_e_offsets[idx] != 0) {
    Elf64_Shdr *n_shdr = TO_HDR(idx);
    Elf64_Shdr *n_strtab = TO_STRTAB(idx);
    Elf64_Sym *n_sym = TO_SYM(idx);
    char *sym_str = (char *)n_strtab->sh_addr + n_sym->st_name;

    if (strcmp(str, sym_str) == 0) {
      *r_hdr = n_shdr;
      *r_sym = n_sym;
      return 0;
    }
  }

  return -1;
}

int symboldb_findmatch(Elf64_Shdr *strhdr, Elf64_Shdr *hdr, Elf64_Sym *sym,
                       Elf64_Shdr **r_hdr, Elf64_Sym **r_sym) {
  // compute the hash, check the entry
  // if the name matches, return the result

  if (symbol_cnt == 0)
    return -1;

  // Get the symbol name
  char *sym_str = (char *)strhdr->sh_addr + sym->st_name;

  // hash and modulus table len
  uint32_t s_hash = hash(sym_str, strlen(sym_str));
  uint32_t idx = s_hash % MAX_SYMBOL_CNT;

  if (symboldb_entrymatch(idx, strhdr, hdr, sym, r_hdr, r_sym) == 0)
    return 0;

  s_hash = murmur3_32((char *)&sym_str, strlen(sym_str),
                      sym_str[0] + strlen(sym_str));
  idx = s_hash % MAX_SYMBOL_CNT;

  if (symboldb_entrymatch(idx, strhdr, hdr, sym, r_hdr, r_sym) == 0)
    return 0;

  DEBUG_PRINT(sym_str);
  PANIC("CRITICAL ERROR: Could not find symbol.");
  return -1;
}