#include "SysReg/registry.h"
#include "SysPhysicalMemory/phys_mem.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>

// Compute the space needed for three levels of bitmaps, 1GiB, 2MiB, 4KiB and
// allocate them
// For the higher levels, 1GiB and 2MiB, allocate an additional bitmap, use to
// determine if the subtree is in use

// If SUBMP_PRESENT, don't touch, treat as used for allocations, recurse for
// free

// If SUBMP_ABSENT, may touch, further state depends on FREE/USED

#define DISTRIBUTE(x) ((x == 0) ? 0 : ~0)

#define FREE (0)
#define USED (1)

#define GET_BIT(src, bit_idx) ((src[bit_idx / 64] >> (bit_idx % 64)) & 1)
#define SET_BIT(src, bit_idx, bit)                                             \
  src[bit_idx / 64] &= ~(1ul << (bit_idx % 64));                               \
  src[bit_idx / 64] |= (1ul << (bit_idx % 64))

static uint64_t *btm_level_bmp;

// Bit sizes
static uint32_t btm_level_sz;

// Byte sizes
static uint32_t btm_sz;

static uint64_t btm_prev_pos = 0;

static uint64_t roundUp_po2(uint64_t val, uint64_t mult) {
  return (val + (mult - 1)) & ~(mult - 1);
}

void pagealloc_free(uint64_t addr, uint64_t size) {

  if (addr % BTM_LEVEL != 0)
    PANIC("Misaligned address");

  if (size % BTM_LEVEL != 0)
    PANIC("Misaligned size");

  btm_prev_pos = addr / BTM_LEVEL;

  while (size > 0) {
    if ((addr % BTM_LEVEL == 0) && (size >= BTM_LEVEL)) {
      // Mark free at the btm level
      SET_BIT(btm_level_bmp, addr / BTM_LEVEL, FREE);

      addr += BTM_LEVEL;
      size -= BTM_LEVEL;
      continue;
    }

    PANIC("Error: Unexpected path.\r\n");
  }
}

static uint64_t data_multiple;
static uint64_t instr_multiple;
static uint64_t pagetable_multiple;

uintptr_t pagealloc_alloc(int domain, int color, physmem_alloc_flags_t flags, uint64_t size) { 

  domain = 0;

  //determine how to compute cache coloring
  // Get associated cache size
  // Divide by page size to get the number of slots
  // Divide by the number of associativity to get the number of colors
  // Page index % number of colors to get the page color
  // Thus, change the page increment appropriately
  // Coloring only matters for single page allocations of cache age size
  
  uint64_t page_multiple = 1;     //Depends on the color
  uint64_t page_base_offset = 0;  //Depends on the domain
  uint64_t cache_unit_sz = 0;

    //allocations are multiples of BTM_LEVEL pages
    size = roundUp_po2(size, BTM_LEVEL);
    cache_unit_sz = BTM_LEVEL;
  
  //Determine search rules
  if(flags & physmem_alloc_flags_data) {
    page_multiple = data_multiple;
  } else if(flags & physmem_alloc_flags_instr) {
    page_multiple = instr_multiple;
  } else if(flags & physmem_alloc_flags_pagetable) {
    page_multiple = pagetable_multiple;
  }

  int32_t btm_pg_off = -1;
  if(cache_unit_sz == BTM_LEVEL){
    
    uint64_t u64_pos = page_base_offset / 64;
    uint64_t bit_pos = page_base_offset % 64;

    uint64_t score = 0;

    //Use a queue of 2MiB pages
    //8 bytes: page address | number of continuous pages

    //Break a 2MiB page into 4KiB pages when more 4KiB pages are needed

    //Sort and merge page sets when enough pages have been freed
    //Commit any 4KiB blocks that reach 2MiB in size to 2MiB queue

    return (uintptr_t)btm_pg_off * BTM_LEVEL;
  }

  return 0; 
}

int pagealloc_init() {

  // initialize allocator as normal
  uint64_t mem_size = 0;
  if (registry_readkey_uint("HW/BOOTINFO", "MEMSIZE", &mem_size) !=
      registry_err_ok)
    PANIC("Failed to read registry.");

  // Compute the number of bits
  btm_sz = roundUp_po2(mem_size, BTM_LEVEL) / BTM_LEVEL;
  btm_level_sz = (uint32_t)btm_sz;

  // Convert bits to uint64 for allocation
  btm_sz = roundUp_po2(btm_sz, 64) / 64;

  // Allocate the blocks
  btm_level_bmp = malloc(btm_sz * sizeof(uint64_t));

  // Mark all memory as 'in-use'
  memset(btm_level_bmp, DISTRIBUTE(USED), btm_sz * sizeof(uint64_t));

  // parse each memory map entry and free the regions
  {
    uint64_t entry_cnt = 0;
    if (registry_readkey_uint("HW/PHYS_MEM", "ENTRY_COUNT", &entry_cnt) !=
        registry_err_ok)
      PANIC("Failed to read registry.");

    for (uint64_t i = 0; i < entry_cnt; i++) {
      char idx_str[10];
      char key_str[256] = "HW/PHYS_MEM/";
      char *key_idx = strncat(key_str, itoa(i, idx_str, 16), 255);

      uint64_t addr = 0;
      uint64_t len = 0;

      if (registry_readkey_uint(key_str, "ADDR", &addr) != registry_err_ok)
        PANIC("Failed to read registry.");

      if (registry_readkey_uint(key_str, "LEN", &len) != registry_err_ok)
        PANIC("Failed to read registry.");

      if (len % BTM_LEVEL != 0)
        len -= len % BTM_LEVEL;

      pagealloc_free(addr, len);
    }
  }

  // allow color and NUMA information to be supplied later

  // allocate the number of 128MiB pages
  // when an allocation is requested, subdivide a block if the requested
  // amount
  // of space is not available.

  // allocator requirements: coloring, NUMA, contiguous, 32-bit/64-bit,
  // emergency reclaimable allocation

  // need to be able to mix and match any of the above requirements

  // NUMA and 32/64 provide address range
  // coloring and contiguous provide page arrangement
  // allocator also needs to be multithreaded

  // Allocation is binning search, NUMA and 32/64 control bin choice
  // Coloring and contiguous-ness provide the search criteria

  // Bins based on NUMA and 32/64
  // Each free block is a binary tree node
  // Contiguous-ness can be satisfied using a simple tree traversal
  // Coloring can be satisfied by searching for the best fit of the coloring
  // optimizations first

  // A binary tree traversal would be very deep for this, it may be more
  // suitable to do a b-tree traversal

  // Each b-tree node tracks the total memory available to its children
  // B-tree nodes are interleaved addresses to handle coloring? -Kills
  // contiguous allocation

  // With coloring optimizations, the objective is to NOT choose a page with
  // a
  // given color, we may want to be able to specify multiple colors to avoid

  data_multiple = 1;
  instr_multiple = 1;
  pagetable_multiple = 1;
  //TODO: Load these with actual data

  return 0;
}
