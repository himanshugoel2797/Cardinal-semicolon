// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
//
// Tests for the shared Internet-checksum helper used by CoreNetwork.

#include <stdint.h>
#include <string.h>

#include "test_framework.h"
#include "checksum.h"

void test_checksum(void) {
    printf("[checksum]\n");

    // Canonical IPv4 header sample (20 bytes) whose stored checksum is 0xb861.
    uint8_t hdr[20] = {
        0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00,
        0x40, 0x11, 0xb8, 0x61, 0xc0, 0xa8, 0x00, 0x01,
        0xc0, 0xa8, 0x00, 0xc7
    };

    // A correct header (checksum field included) folds to zero.
    CHECK_EQ_U(net_checksum16(hdr, sizeof(hdr)), 0);

    // Generation path: zero the field, recompute, store the native-order result
    // into the uint16 field, and confirm the on-wire bytes are 0xb8,0x61 and
    // that the header now validates to zero. This pins the byte orientation the
    // real ipv4_tx relies on.
    hdr[10] = 0;
    hdr[11] = 0;
    uint16_t csum = net_checksum16(hdr, sizeof(hdr));
    memcpy(&hdr[10], &csum, sizeof(csum));
    CHECK_EQ_U(hdr[10], 0xb8);
    CHECK_EQ_U(hdr[11], 0x61);
    CHECK_EQ_U(net_checksum16(hdr, sizeof(hdr)), 0);

    // Degenerate lengths.
    uint8_t one = 0x12;
    CHECK_EQ_U(net_checksum16(&one, 0), 0xffff);          // sum 0 -> ~0
    CHECK_EQ_U(net_checksum16(&one, 1), (uint16_t)~0x12u);  // odd trailing byte

    // Carry folding: 0xffff + 0xffff = 0x1fffe -> fold 0xffff -> ~ = 0.
    uint8_t ff[4] = {0xff, 0xff, 0xff, 0xff};
    CHECK_EQ_U(net_checksum16(ff, sizeof(ff)), 0);

    // Partial-accumulate API must match the one-shot helper (used for the
    // pseudo-header + payload pattern in future UDP/TCP work).
    uint32_t acc = net_csum_acc(0, hdr, 10);
    acc = net_csum_acc(acc, hdr + 10, sizeof(hdr) - 10);
    CHECK_EQ_U(net_csum_fold(acc), net_checksum16(hdr, sizeof(hdr)));
}
