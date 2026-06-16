// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stddef.h>
#include <stdint.h>

#include "SysTest/test.h"

#include "CoreNetwork/driver.h"
#include "CoreNetwork/net.h"

// Header-length constants must match the on-wire layouts the protocol code
// assumes.
static void test_header_lengths(test_ctx_t *ctx) {
    TEST_CHECK_EQ_U(ctx, ARP_HEADER_LEN, 28);
    TEST_CHECK_EQ_U(ctx, IPV4_HEADER_LEN, 20);
    TEST_CHECK_EQ_U(ctx, UDP_HEADER_LEN, 8);
    TEST_CHECK_EQ_U(ctx, UDP_IPV4_PACKET_SPACE,
                    IPV4_HEADER_LEN + UDP_HEADER_LEN);
}

// Registering an ethernet device with a trivial descriptor should succeed and
// hand back a non-NULL interface handle.
static void test_network_register(test_ctx_t *ctx) {
    network_device_desc_t desc;
    for (size_t i = 0; i < sizeof(desc); i++)
        ((uint8_t *)&desc)[i] = 0;

    desc.name[0] = '\0';
    desc.type = network_device_type_ethernet;
    desc.handlers.ether.tx = NULL;
    desc.handlers.ether.link_status = NULL;
    desc.lock = 0;

    void *handle = NULL;
    int ret = network_register(&desc, &handle);

    TEST_CHECK_EQ_U(ctx, ret, 0);
    TEST_CHECK(ctx, handle != NULL);
}

void corenetwork_register_tests(void) {
    if (!test_mode_active())
        return;

    {
        test_def_t t = {
            .suite = "CoreNetwork",
            .name = "header_lengths",
            .fn = test_header_lengths,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }

    {
        test_def_t t = {
            .suite = "CoreNetwork",
            .name = "network_register",
            .fn = test_network_register,
            .run = TEST_RUN_INLINE,
            .flags = TEST_FLAG_NONE,
        };
        test_register(&t);
    }
}
