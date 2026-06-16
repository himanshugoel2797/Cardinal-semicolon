// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stddef.h>
#include <stdint.h>

#include "SysTest/test.h"

#include "CoreNetwork/driver.h"
#include "CoreNetwork/net.h"

#include "arp.h"
#include "ip.h"
#include "udp.h"

// The *_HEADER_LEN constants are what the protocol code uses to size and offset
// into on-wire buffers; the packed structs are how that same code actually reads
// and writes the fields. The two must agree, so compare the constants against
// the real sizeof() of the wire structs. This catches a struct-layout
// regression (e.g. lost packing, a reordered/resized field) -- comparing a
// constant against its own literal would not.
static void test_header_lengths(test_ctx_t *ctx) {
    // arp_t / ipv4_t carry no trailing payload in the header proper (ipv4_t's
    // body[] is a zero-length flexible member, excluded from sizeof).
    TEST_CHECK_EQ_U(ctx, ARP_HEADER_LEN, sizeof(arp_t));
    TEST_CHECK_EQ_U(ctx, IPV4_HEADER_LEN, sizeof(ipv4_t));
    TEST_CHECK_EQ_U(ctx, UDP_HEADER_LEN, sizeof(udp_t));

    // Concrete wire sizes, so a silently mis-packed struct is caught even if its
    // matching constant drifted with it.
    TEST_CHECK_EQ_U(ctx, sizeof(arp_t), 28);
    TEST_CHECK_EQ_U(ctx, sizeof(ipv4_t), 20);
    TEST_CHECK_EQ_U(ctx, sizeof(udp_t), 8);

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
