#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <types.h>

#include "boot_information.h"
#include "load_script.h"
#include "symbol_db.h"

static int (*debug_handler)() = NULL;
static int (*print_str_handler)(const char *) = NULL;
static void (*set_str_handler)(const char *) = NULL;

static char priv_s[2048];
int WEAK debug_handle_trap() {
    if (debug_handler != NULL)
        return debug_handler();

    const char *p = priv_s;
    print_str(p);
    __asm__("cli\n\thlt");
    return 0;
}
int WEAK print_str(const char *s) {
    if (print_str_handler != NULL)
        return print_str_handler(s);

    while (*s != 0)
        outb(0x3f8, *(s++));

    return 0;
}

void WEAK set_trap_str(const char *s) {
    if (set_str_handler != NULL)
        return set_str_handler(s);
    strncpy(priv_s, s, 2048);
}

int kernel_updatetraphandlers() {
    debug_handler = (int (*)())elf_resolvefunction("debug_handle_trap");
    if (debug_handler == debug_handle_trap)
        debug_handler = NULL;

    print_str_handler = (int (*)(const char *))elf_resolvefunction("print_str");
    if (print_str_handler == print_str)
        print_str_handler = NULL;

    set_str_handler = (void (*)(const char *))elf_resolvefunction("set_trap_str");
    if (set_str_handler == set_trap_str)
        set_str_handler = NULL;

    return 0;
}

SECTION(".entry_point") int32_t main(void *param, uint64_t magic) {

    if (param == NULL)
        PANIC("Didn't receive boot parameters!");

    ParseAndSaveBootInformation(param, magic);

    // Fix boot information addresses
    CardinalBootInfo *b_info = GetBootInfo();
    
    //Move initrd into allocated memory
    b_info->InitrdStartAddress += 0xffffffff80000000;
    //b_info->FramebufferAddress += 0xffffffff80000000;

    uint64_t initrd_copy = (uint64_t)malloc(b_info->InitrdLength + 512);
    if(initrd_copy % 512 != 0)
        initrd_copy += 512 - (initrd_copy % 512);

    memcpy((void*)initrd_copy, (void*)b_info->InitrdStartAddress, b_info->InitrdLength);
    memset((void*)b_info->InitrdStartAddress, 0, b_info->InitrdLength);
    b_info->InitrdStartAddress = initrd_copy;

    // Initalize and load
    symboldb_init();
    elf_installkernelsymbols();
    loadscript_execute();

    PANIC("KERNEL END REACHED.");
    while (1)
        ;
    return 0;
}

SECTION(".tramp_handler") uint64_t tramp_stack = 0xffffffff80000000;

void alloc_ap_stack(void) {
    uint64_t stack = (uint64_t)malloc(4096);
    tramp_stack = stack + 4096;
}

SECTION(".tramp_handler") void smp_bootstrap(void) {
    apscript_execute();
    while (1)
    ;
}