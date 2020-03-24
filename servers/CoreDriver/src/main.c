/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <string.h>
#include <stdlib.h>

#include "SysReg/registry.h"
#include "module_lib/module_def.h"

#include "initrd.h"
#include "elf.h"

int module_init()
{

    char *str = NULL;
    size_t str_sz = 0;
    if (!Initrd_GetFile("./devices.txt", (void **)&str, &str_sz))
        return -1;

    //PCI device info
    uint64_t deviceCount = 0;
    if (registry_readkey_uint("HW/PCI", "COUNT", &deviceCount) != registry_err_ok)
        return -1;

    const char *cursor = str;
    const char *end = str + str_sz;
    while (cursor < end)
    {
        const char *exec_str = cursor;
        const char *vendorID_str = strchr(cursor, '|') + 1;
        const char *devID_str = strchr(vendorID_str, '|') + 1;
        const char *class_str = strchr(devID_str, '|') + 1;
        const char *subclass_str = strchr(class_str, '|') + 1;
        const char *progif_str = strchr(subclass_str, '|') + 1;

        const char *newline = strchr(cursor, '\n');

        int vendorID = atoi(vendorID_str, 16);
        int devID = atoi(devID_str, 16);
        int class = atoi(class_str, 16);
        int subclass = atoi(subclass_str, 16);
        int progif = atoi(progif_str, 16);

        for (uint64_t idx = 0; idx < deviceCount; idx++)
        {
            char idx_str[10] = "";
            char key_str[256] = "HW/PCI/";
            char *key_idx = strncat(key_str, itoa(idx, idx_str, 16), 255);

            uint64_t dev_class = 0;
            if (registry_readkey_uint(key_idx, "CLASS", &dev_class) != registry_err_ok)
                return -1;

            uint64_t dev_subclass = 0;
            if (registry_readkey_uint(key_idx, "SUBCLASS", &dev_subclass) != registry_err_ok)
                return -1;

            uint64_t dev_devID = 0;
            if (registry_readkey_uint(key_idx, "DEVICE_ID", &dev_devID) != registry_err_ok)
                return -1;

            uint64_t dev_vendorID = 0;
            if (registry_readkey_uint(key_idx, "VENDOR_ID", &dev_vendorID) != registry_err_ok)
                return -1;

            bool classMatch = (class == (int)dev_class) || (class == 0xFFFF);
            bool subclassMatch = (subclass == (int)dev_subclass) || (subclass == 0xFFFF);
            bool devIDMatch = (devID == (int)dev_devID) || (devID == 0xFFFF);
            bool vendorIDMatch = (vendorID == (int)dev_vendorID) || (vendorID == 0xFFFF);

            if (classMatch && subclassMatch && devIDMatch && vendorIDMatch)
            {

                char *tmp = (char *)strchr(cursor, '|');
                *tmp = 0;

                //load this driver
                print_str("[CoreDriver] Load module:");
                print_str(exec_str);
                print_str("\r\n");

                void *mod_loc = NULL;
                size_t len = 0;
                if (!Initrd_GetFile(exec_str, &mod_loc, &len))
                    PANIC("[CoreDriver] Failed to find module!");

                // decompress celf's elf section
                ModuleHeader *hdr = (ModuleHeader *)mod_loc;

                int (*entry_pt)() = NULL;
                if (elf_load(hdr->data, hdr->uncompressed_len, &entry_pt))
                    PANIC("[CoreDriver] Elf load failed");

                int (*entry_pt_real)(void *) = (int (*)(void *))entry_pt;

                uint64_t ecam_addr = 0;
                if (registry_readkey_uint(key_idx, "ECAM_ADDR", &ecam_addr) != registry_err_ok)
                    return -1;

                char tmp_entry_addr[20];
                print_str("[CoreDriver] Device ECAM at ");
                print_str(ltoa(ecam_addr, tmp_entry_addr, 16));
                print_str("\r\n");

                print_str("[CoreDriver] LOADED at ");
                print_str(ltoa((uint64_t)entry_pt, tmp_entry_addr, 16));
                print_str("\r\n");

                int err = entry_pt_real((void *)ecam_addr);
                char idx_str[10];
                print_str("[CoreDriver] Return value:");
                print_str(itoa(err, idx_str, 16));
                print_str("\r\n");
                if (err != 0)
                {
                    PANIC("[CoreDriver] module_init FAILED");
                }
            }
        }

        cursor = newline + 1;
    }

    return 0;
}