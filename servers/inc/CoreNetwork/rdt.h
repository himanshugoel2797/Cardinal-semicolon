// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SEMI_CORENETWORK_RDT_H
#define CARDINAL_SEMI_CORENETWORK_RDT_H

#include <stdint.h>
#include <types.h>

// RDT -- a minimal Reliable Delivery Transport over UDP.
//
// Goal: get a named blob to the device reliably over a lossy link, much faster
// than the serial bridge, with no device-side timers. The protocol is
// responder-driven and stop-and-wait: the *host* drives all retransmission and
// the device only ever ACKs what it has, so a dropped datagram costs one host
// timeout, never a device hang. It mirrors the serial recv_blob design, lifted
// onto UDP.
//
// One datagram carries one rdt_hdr_t, immediately followed by:
//   START : `name_len` bytes of the blob name (no NUL on the wire)
//   DATA  : `chunk_len` bytes of blob payload at byte `offset`
//   ACK   : nothing (device -> host only)
//
// Exchange (host -> device unless noted):
//   START{xfer_id, total_len, name}     device allocates a buffer, ACK{offset=0}
//   DATA {xfer_id, offset=recd, chunk}  device appends, ACK{offset=recd'}
//     (offset != recd -> device re-ACKs its current recd, no copy: idempotent)
//   ... until recd == total_len ...
//   on completion the device runs the listener callback, then sends a final
//   ACK with RDT_FLAG_FIN set. A retransmitted final DATA re-ACKs FIN without
//   re-running the callback.
//
// Multi-byte header fields are network byte order. The checksum covers the
// header (with its own 2 csum bytes excluded) plus the trailing bytes, as a
// standard internet checksum (see servers/CoreNetwork/inc/checksum.h), and is
// itself stored network order.

#define RDT_MAGIC0 'C'
#define RDT_MAGIC1 'R'
#define RDT_MAGIC2 'D'
#define RDT_MAGIC3 'T'

enum {
    RDT_TYPE_START = 1,
    RDT_TYPE_DATA = 2,
    RDT_TYPE_ACK = 3,
};

#define RDT_FLAG_FIN (1u << 0)  // set on the ACK that completes a transfer

typedef struct {
    uint8_t magic[4];    // 'C','R','D','T'
    uint8_t type;        // RDT_TYPE_*
    uint8_t flags;       // RDT_FLAG_*
    uint16_t name_len;   // START: name bytes following the header; else 0
    uint32_t xfer_id;    // host-chosen transfer id
    uint32_t total_len;  // total blob length in bytes
    uint32_t offset;     // DATA: chunk's byte offset; ACK: cumulative bytes received
    uint16_t chunk_len;  // DATA: payload bytes following the header; else 0
    uint16_t csum;       // internet checksum (see header comment); MUST be last
} PACKED rdt_hdr_t;

// Largest blob a single transfer may declare (bounds device memory against a
// hostile START). Largest blob name. Both are protocol caps the host must obey.
#define RDT_MAX_BLOB (16 * 1024 * 1024)
#define RDT_MAX_NAME 64

// Callback invoked once, in the receive path, when a blob has fully arrived.
// `iface`/`src_ip`/`src_mac`/`src_port` identify the sender; `name` is a
// NUL-terminated copy (owned by the transport, valid only for the call);
// `data`/`len` are the assembled blob (owned by the transport, freed after the
// call returns). Do not retain `name`/`data` past the callback.
typedef void (*rdt_complete_t)(void *ctx, void *iface, uint32_t src_ip,
                               const uint8_t *src_mac, uint16_t src_port,
                               const char *name, void *data, int len);

// Listen for RDT transfers on UDP `port`. Returns 0 on success, -1 on bad args,
// no free listener slot, or if the underlying udp_bind fails (e.g. port taken).
int rdt_listen(uint16_t port, rdt_complete_t on_complete, void *ctx);

#endif
