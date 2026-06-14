// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINALSEMI_CORENETWORK_CHECKSUM_H
#define CARDINALSEMI_CORENETWORK_CHECKSUM_H

#include <stdint.h>

// Internet checksum (RFC 1071) helpers.
//
// The one's-complement sum is computed over the buffer treated as a sequence of
// native-order 16-bit words. Because the fold/complement commutes with a
// byte-swap of every word, a checksum generated and stored in native order
// validates correctly on a peer of either endianness -- the only thing a
// receiver checks is that the sum over the whole structure (including the stored
// checksum field) folds to zero. Compute on this little-endian host, store the
// result directly into the (native) checksum field, and verification is
// "net_checksum16(buf, len) == 0".

// Accumulate `len` bytes into a running one's-complement sum. May be called
// repeatedly (e.g. a pseudo-header followed by the payload) as long as every
// chunk except possibly the last has even length.
static inline uint32_t net_csum_acc(uint32_t sum, const void *data, int len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len > 1) {
        // Reassemble the native-order 16-bit word without assuming alignment.
        sum += (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        p += 2;
        len -= 2;
    }
    if (len > 0)  // trailing odd byte occupies the low half of a zero-padded word
        sum += (uint32_t)p[0];
    return sum;
}

// Fold a 32-bit accumulator down to the 16-bit one's-complement checksum.
static inline uint16_t net_csum_fold(uint32_t sum) {
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static inline uint16_t net_checksum16(const void *data, int len) {
    return net_csum_fold(net_csum_acc(0, data, len));
}

#endif
