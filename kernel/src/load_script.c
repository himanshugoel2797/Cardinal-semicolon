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

    // decompress celf's elf section
    ModuleHeader *hdr = (ModuleHeader *)mod_loc;

    int (*entry_pt)() = NULL;
    if (elf_load(hdr->data, hdr->uncompressed_len, &entry_pt))
        PANIC("[Kernel] Elf load failed.");

    char tmp_entry_addr[20];
    print_str("[Kernel] Loaded at ");
    print_str(ltoa((uint64_t)hdr->data, tmp_entry_addr, 16));
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

int loadscript_execute()
{

    char *load_script = NULL;
    size_t load_len = 0;
    if (!Initrd_GetFile("./loadscript.txt", (void **)&load_script, &load_len))
        PANIC("[Kernel] Failed to find loadscript.");

    return script_execute(load_script, load_len);
}

int servicescript_execute()
{

    char *load_script = NULL;
    size_t load_len = 0;
    if (!Initrd_GetFile("./servicescript.txt", (void **)&load_script, &load_len))
        PANIC("[Kernel] Failed to find servicescript.");

    return script_execute(load_script, load_len);
}

int apscript_execute()
{

    char *load_script = NULL;
    size_t load_len = 0;
    if (!Initrd_GetFile("./apscript.txt", (void **)&load_script, &load_len))
        PANIC("[Kernel] Failed to find apscript.");

    return script_execute(load_script, load_len);
}