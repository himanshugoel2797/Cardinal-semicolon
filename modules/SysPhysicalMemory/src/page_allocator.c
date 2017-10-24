#define LVL0_SZ MiB(128)
#define LVL1_SZ MiB(2)
#define LVL2_SZ KiB(4)

int pagealloc_init() {

    // obtain NUMA domain info and divide memory among the domains
    // allow allocations only within domains

    // allocate the number of 128MiB pages
    // when an allocation is requested, subdivide a block if the requested amount
    // of space is not available.

    return 0;
}
