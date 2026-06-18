// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <types.h>

#include "boot_information.h"
#include "CoreNetwork/udp.h"
#include "CoreNetwork/rdt.h"

// CoreNetDebug -- an optional, cmdline-gated network debug endpoint built on the
// general UDP + RDT facilities CoreNetwork exports. It gives a swap-free, far-
// faster-than-serial way to poke the running OS:
//
//   UDP 1337 (echo)   : every datagram is bounced straight back to the sender.
//                       Liveness + round-trip latency.
//   UDP 1338 (upload) : a reliable named-blob receiver (RDT). On completion the
//                       blob is summarized + digested to the debug console so a
//                       host tool can confirm integrity end-to-end.
//
// It is inert unless the kernel command line carries "cardinal.netdbg" -- this
// is a remote attack surface, so it is opt-in just like the serial debug shell.

#define NETDBG_ECHO_PORT 1337
#define NETDBG_UPLOAD_PORT 1338

static void echo_handler(void *ctx, void *iface, uint32_t src_ip,
                         const uint8_t *src_mac, uint16_t src_port,
                         uint16_t dst_port, const void *payload, int len) {
    (void)ctx;
    (void)src_ip;
    // Bounce the payload back to where it came from (responder-driven: we reply
    // to the captured src_mac, so no ARP resolution is needed).
    udp_send_to(iface, src_ip, src_mac, dst_port, src_port, payload, len);
}

static void on_upload(void *ctx, void *iface, uint32_t src_ip,
                      const uint8_t *src_mac, uint16_t src_port,
                      const char *name, void *data, int len) {
    (void)ctx;
    (void)iface;
    (void)src_ip;
    (void)src_mac;
    (void)src_port;

    // A small order-sensitive digest so the host can confirm the blob arrived
    // intact (not just at the right length). Cheap and dependency-free.
    uint32_t digest = 2166136261u;  // FNV-1a basis
    const uint8_t *p = (const uint8_t *)data;
    for (int i = 0; i < len; i++) {
        digest ^= p[i];
        digest *= 16777619u;
    }

    char tmp[16];
    DEBUG_PRINT("[CoreNetDebug] upload complete: name='");
    DEBUG_PRINT(name);
    DEBUG_PRINT("' len=");
    DEBUG_PRINT(itoa(len, tmp, 10));
    DEBUG_PRINT(" digest=0x");
    DEBUG_PRINT(itoa((int)digest, tmp, 16));
    DEBUG_PRINT("\r\n");
}

int module_init() {
    CardinalBootInfo *bi = GetBootInfo();
    if (bi == NULL || strstr(bi->Cmdline, "cardinal.netdbg") == NULL)
        return 0;  // not requested -> stay inert

    if (udp_bind(NETDBG_ECHO_PORT, echo_handler, NULL) != 0)
        DEBUG_PRINT("[CoreNetDebug] failed to bind echo port\r\n");

    if (rdt_listen(NETDBG_UPLOAD_PORT, on_upload, NULL) != 0)
        DEBUG_PRINT("[CoreNetDebug] failed to listen on upload port\r\n");

    DEBUG_PRINT("[CoreNetDebug] enabled (echo 1337, upload 1338)\r\n");
    return 0;
}
