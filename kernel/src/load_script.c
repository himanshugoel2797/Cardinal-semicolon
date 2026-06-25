#include "load_script.h"
#include "elf.h"
#include "module_def.h"

#include "initrd.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <types.h>

int module_load(char *name)
{
    print_str("[Kernel] Load module:");
    print_str(name);
    print_str("\r\n");

    void *mod_loc = NULL;
    size_t len = 0;
    if (!Initrd_GetFile(name, &mod_loc, &len))
        PANIC("[Kernel] Failed to find module!");

    ModuleHeader *hdr = (ModuleHeader *)mod_loc;

    // elf_load places each section in-place at base + sh_offset and relies on it
    // landing at the alignment the linker assumed -- e.g. .rodata is 16-aligned so
    // optimized (-O2) module code can use 16-byte SSE moves (movaps) against
    // constants there. The initrd only aligns hdr->data to 8 bytes, so a movaps on
    // a "16-aligned" constant #GPs. Copy the ELF to a buffer aligned past any
    // section requirement (64 covers SSE/AVX + cache lines) before loading it.
    enum { MOD_ALIGN = 64 };
    uint8_t *raw = (uint8_t *)malloc((size_t)hdr->uncompressed_len + MOD_ALIGN);
    if (raw == NULL)
        PANIC("[Kernel] module load buffer alloc failed.");
    uint8_t *elf =
        (uint8_t *)(((uintptr_t)raw + (MOD_ALIGN - 1)) & ~(uintptr_t)(MOD_ALIGN - 1));
    memcpy(elf, hdr->data, hdr->uncompressed_len);

    int (*entry_pt)() = NULL;
    if (elf_load(elf, hdr->uncompressed_len, &entry_pt))
        PANIC("[Kernel] Elf load failed.");

    char tmp_entry_addr[20];
    print_str("[Kernel] Loaded at ");
    print_str(ltoa((uint64_t)elf, tmp_entry_addr, 16));
    print_str("\r\n");

    int err = entry_pt();
    return err;
}

void module_user_load(char *name)
{
    print_str("[Kernel] Load user module:");
    print_str(name);
    print_str("\r\n");

    void *mod_loc = NULL;
    size_t len = 0;
    if (!Initrd_GetFile(name, &mod_loc, &len))
        PANIC("[Kernel] Failed to find module!");

    // decompress celf's elf section
    ModuleHeader *hdr = (ModuleHeader *)mod_loc;
    void (*task_startnew_user)(void *, size_t) = (void (*)(void *, size_t))elf_resolvefunction("task_startnew_user");
    task_startnew_user(hdr->data, hdr->uncompressed_len);
}

int script_execute(char *load_script, size_t load_len)
{
    char name[1024];
    bool isCRLF = false;

    uintptr_t load_script_end = (uintptr_t)(load_script + load_len);
    while ((uintptr_t)load_script < load_script_end)
    {

        int mode = -1;

        if (strncmp(load_script, "LOAD:", 5) == 0)
            mode = 0;
        else if (strncmp(load_script, "CALL:", 5) == 0)
            mode = 1;
        else if (strncmp(load_script, "USER:", 5) == 0)
            mode = 2;
        else if (strncmp(load_script, "#", 1) == 0)
            mode = -2;

        load_script += 5;
        const char *end_of_line = strstr(load_script, "\n");
        memset(name, 0, 1024);

        //handle both line endings to avoid annoying issues during development
        if (*(end_of_line - 1) == '\r')
        {
            isCRLF = true;
            end_of_line--;
        }

        strncpy(name, load_script, (size_t)(end_of_line - load_script));
        load_script += end_of_line - load_script + 1;
        if (isCRLF)
            load_script++;

        if (mode == 0)
        {

            int err = module_load(name);
            if (err != 0)
            {
                char idx_str[10];
                print_str("[Kernel] Return value:");
                print_str(itoa(err, idx_str, 16));
                print_str("\r\n");
                PANIC("[Kernel] module_init failed");
            }
        }
        else if (mode == 1)
        {
            print_str("[Kernel] Call function:");
            print_str(name);
            print_str("\r\n");

            int (*entry_pt)() = (int (*)())elf_resolvefunction(name);
            if (entry_pt == NULL)
                PANIC("[Kernel] Failed to resolve function!");

            int err = entry_pt();
            if (err != 0)
            {
                char idx_str[10];
                print_str("[Kernel] Return value:");
                print_str(itoa(err, idx_str, 16));
                print_str("\r\n");
                PANIC("[Kernel] Call failed");
            }
        }
        else if (mode == 2)
        {
            module_user_load(name);
        }
        else if (mode == -1)
        {
            print_str("[Kernel] Name:");
            print_str(name);

            print_str("\r\n[Kernel] Load script:");
            print_str(load_script);
            PANIC("[Kernel] Unknown Command");
        }
    }
    return 0;
}

static int named_script_execute(const char *filename)
{
    char *load_script = NULL;
    size_t load_len = 0;
    if (!Initrd_GetFile(filename, (void **)&load_script, &load_len))
        PANIC("[Kernel] Failed to find script.");

    return script_execute(load_script, load_len);
}

int loadscript_execute()
{
    return named_script_execute("./loadscript.txt");
}

int apscript_execute()
{
    return named_script_execute("./apscript.txt");
}