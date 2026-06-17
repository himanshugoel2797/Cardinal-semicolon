// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <types.h>

#include "SysTest/test.h"
#include "SysReg/registry.h"

// CoreDriver is the boot-time PCI driver loader: module_init walks devices.txt
// and, for each PCI device the kernel enumerated under "HW/PCI", reads its
// CLASS/SUBCLASS/INTERFACE/DEVICE_ID/VENDOR_ID/ECAM_ADDR registry keys to
// decide which driver .celf to bind. These tests assert the registry
// preconditions CoreDriver actually consumes -- the same keys module_init
// reads. If PCI enumeration regressed, module_init would early-return -1 and
// the driver stack would never come up, so a tautology here ("1 == 1") proved
// nothing.

// The PCI device count must be present in the registry; module_init bails with
// -1 if it cannot read it.
static void test_pci_count_present(test_ctx_t *ctx)
{
    uint64_t count = 0;
    TEST_CHECK_EQ_U(ctx, registry_readkey_uint("HW/PCI", "COUNT", &count), CS_OK);
}

// Each enumerated device must carry every key module_init reads when matching a
// devices.txt entry. Validate device 0 (the key is constructed exactly as
// module_init does: "HW/PCI/" + hex index, which is "HW/PCI/0" for index 0).
static void test_pci_device0_keys(test_ctx_t *ctx)
{
    uint64_t count = 0;
    TEST_CHECK_EQ_U(ctx, registry_readkey_uint("HW/PCI", "COUNT", &count), CS_OK);
    if (count == 0)
        return;  // no PCI devices enumerated -- still a valid configuration

    const char *key = "HW/PCI/0";
    uint64_t v = 0;
    TEST_CHECK_EQ_U(ctx, registry_readkey_uint(key, "CLASS", &v), CS_OK);
    TEST_CHECK_EQ_U(ctx, registry_readkey_uint(key, "SUBCLASS", &v), CS_OK);
    TEST_CHECK_EQ_U(ctx, registry_readkey_uint(key, "INTERFACE", &v), CS_OK);
    TEST_CHECK_EQ_U(ctx, registry_readkey_uint(key, "DEVICE_ID", &v), CS_OK);
    TEST_CHECK_EQ_U(ctx, registry_readkey_uint(key, "VENDOR_ID", &v), CS_OK);
    TEST_CHECK_EQ_U(ctx, registry_readkey_uint(key, "ECAM_ADDR", &v), CS_OK);
}

void coredriver_register_tests(void)
{
    if (!test_mode_active())
        return;

    test_def_t count_present = {
        .suite = "CoreDriver",
        .name = "pci_count_present",
        .fn = test_pci_count_present,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&count_present);

    test_def_t device0_keys = {
        .suite = "CoreDriver",
        .name = "pci_device0_keys",
        .fn = test_pci_device0_keys,
        .run = TEST_RUN_INLINE,
        .flags = TEST_FLAG_NONE,
    };
    test_register(&device0_keys);
}
