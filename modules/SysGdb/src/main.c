/**
 * Copyright (c) 2026 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 *
 * SysGdb -- a GDB Remote Serial Protocol stub for debugging the OS over a serial
 * channel (even on real hardware). It registers handlers for the breakpoint
 * (#BP) and debug (#DB) exceptions; when one fires it talks RSP to GDB over a
 * serial channel: read/write registers (g/G/p/P), read/write memory (m/M),
 * single-step (s) and continue (c). GDB drives software breakpoints itself via
 * memory writes + single-step, so no Zx packets are needed.
 *
 * The channel defaults to COM2 (0x2F8) so the debug console on COM1 is
 * undisturbed; a driver (e.g. USB-serial) can replace it at runtime via
 * gdb_register_transport(), letting the same stub run over any byte transport.
 * The transport only provides byte I/O -- all RSP protocol and the async
 * break-in decision stay here, so transport drivers carry no GDB knowledge.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <types.h>

#include "SysInterrupts/interrupts.h"

#include "SysGdb/gdb.h"

// ---- Serial channel (pluggable transport; defaults to COM2) ----
#define COM2 0x2F8

static void com2_init(void) {
    outb(COM2 + 1, 0x00);  // disable UART interrupts
    outb(COM2 + 3, 0x80);  // DLAB
    outb(COM2 + 0, 0x01);  // divisor low = 1 -> 115200 baud
    outb(COM2 + 1, 0x00);  // divisor high
    outb(COM2 + 3, 0x03);  // 8 bits, no parity, 1 stop
    outb(COM2 + 2, 0xC7);  // enable + clear FIFO
    outb(COM2 + 4, 0x0B);  // RTS/DSR set
}
static int com2_getc(void *state) {
    (void)state;
    while (!(inb(COM2 + 5) & 0x01))
        ;
    return inb(COM2);
}
static void com2_putc(void *state, int c) {
    (void)state;
    while (!(inb(COM2 + 5) & 0x20))
        ;
    outb(COM2, (uint8_t)c);
}

// The active transport. COM2 has no `poll`: it delivers async break-in via its
// UART RX IRQ (gdb_com2_rx) instead.
static gdb_transport_t cur_transport = {
    .getc = com2_getc,
    .putc = com2_putc,
    .poll = NULL,
    .state = NULL,
};

static inline int chan_getc(void) { return cur_transport.getc(cur_transport.state); }
static inline void chan_putc(int c) { cur_transport.putc(cur_transport.state, c); }

// The built-in COM2 transport, kept so a transport can be torn down (e.g. a
// USB-serial adapter unplugged) and the channel reverts to COM2.
static const gdb_transport_t com2_transport = {
    .getc = com2_getc,
    .putc = com2_putc,
    .poll = NULL,
    .state = NULL,
};

// A driver takes over the GDB channel at runtime. Copied so the caller need not
// keep the descriptor alive.
void gdb_register_transport(const gdb_transport_t *t) {
    if (t == NULL || t->getc == NULL || t->putc == NULL)
        return;
    cur_transport = *t;
    DEBUG_PRINT("[SysGdb] GDB channel switched to a registered transport\r\n");
}

// Revert the GDB channel to the built-in COM2 transport. `t`, when non-NULL,
// only restores the default if it is still the active transport (so a stale
// driver unplug cannot clobber a newer transport). Pass NULL to force it.
void gdb_unregister_transport(const gdb_transport_t *t) {
    if (t != NULL &&
        (cur_transport.getc != t->getc || cur_transport.state != t->state))
        return;  // a different transport is active; leave it alone
    cur_transport = com2_transport;
    DEBUG_PRINT("[SysGdb] GDB channel reverted to COM2\r\n");
}

// ---- RSP helpers ----
static const char hexc[] = "0123456789abcdef";

static int fromhex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

#define GDB_BUF 1024
static char rx_buf[GDB_BUF];
static char tx_buf[GDB_BUF];

// Receive one packet ($...#cs) into rx_buf; returns length or -1 on bad checksum.
static int recv_packet(void) {
    int c;
    do {
        c = chan_getc();
    } while (c != '$');

    int len = 0;
    uint8_t sum = 0;
    while (1) {
        c = chan_getc();
        if (c == '#')
            break;
        if (len < GDB_BUF - 1) {
            rx_buf[len++] = (char)c;
            sum += (uint8_t)c;
        }
    }
    rx_buf[len] = 0;
    int h1 = fromhex(chan_getc());
    int h2 = fromhex(chan_getc());
    if (((h1 << 4) | h2) == sum) {
        chan_putc('+');
        return len;
    }
    chan_putc('-');
    return -1;
}

static void send_packet(const char *data) {
    for (int retry = 0; retry < 10; retry++) {
        chan_putc('$');
        uint8_t sum = 0;
        for (const char *p = data; *p; p++) {
            chan_putc(*p);
            sum += (uint8_t)*p;
        }
        chan_putc('#');
        chan_putc(hexc[(sum >> 4) & 0xf]);
        chan_putc(hexc[sum & 0xf]);
        int a = chan_getc();
        if (a == '+')
            return;
        if (a == '$') {
            // GDB sent a new packet instead of acking; treat as ack and bail so
            // the caller re-reads. (Rare; avoids deadlock.)
            return;
        }
        // otherwise ('-' or noise) resend
    }
}

static char *put_hex_bytes(char *p, const void *data, int n) {
    const uint8_t *b = (const uint8_t *)data;
    for (int i = 0; i < n; i++) {
        *p++ = hexc[(b[i] >> 4) & 0xf];
        *p++ = hexc[b[i] & 0xf];
    }
    return p;
}

// Parse a hex number from *pp, advancing it; stops at non-hex.
static uint64_t parse_hex(const char **pp) {
    uint64_t v = 0;
    int d;
    while ((d = fromhex(**pp)) >= 0) {
        v = (v << 4) | (uint32_t)d;
        (*pp)++;
    }
    return v;
}

// ---- Register packet (GDB x86_64 g/G order) ----
// rax rbx rcx rdx rsi rdi rbp rsp r8..r15 (8 bytes each), rip (8),
// eflags cs ss ds es fs gs (4 bytes each).
static void build_g_packet(interrupt_register_state_t *st, char *out) {
    char *p = out;
    uint64_t regs[16] = {st->rax, st->rbx, st->rcx, st->rdx, st->rsi, st->rdi,
                         st->rbp, st->rsp, st->r8,  st->r9,  st->r10, st->r11,
                         st->r12, st->r13, st->r14, st->r15};
    for (int i = 0; i < 16; i++)
        p = put_hex_bytes(p, &regs[i], 8);
    p = put_hex_bytes(p, &st->rip, 8);
    uint32_t eflags = (uint32_t)st->rflags;
    uint32_t cs = (uint32_t)st->cs, ss = (uint32_t)st->ss, seg = 0;
    p = put_hex_bytes(p, &eflags, 4);
    p = put_hex_bytes(p, &cs, 4);
    p = put_hex_bytes(p, &ss, 4);
    p = put_hex_bytes(p, &seg, 4);  // ds
    p = put_hex_bytes(p, &seg, 4);  // es
    p = put_hex_bytes(p, &seg, 4);  // fs
    p = put_hex_bytes(p, &seg, 4);  // gs
    *p = 0;
}

static void parse_G_packet(interrupt_register_state_t *st, const char *in) {
    uint64_t v[16];
    const char *p = in;
    for (int i = 0; i < 16; i++) {
        uint64_t val = 0;
        for (int b = 0; b < 8; b++) {
            int h1 = fromhex(*p++), h2 = fromhex(*p++);
            if (h1 < 0 || h2 < 0)
                return;
            val |= ((uint64_t)((h1 << 4) | h2)) << (b * 8);
        }
        v[i] = val;
    }
    st->rax = v[0]; st->rbx = v[1]; st->rcx = v[2]; st->rdx = v[3];
    st->rsi = v[4]; st->rdi = v[5]; st->rbp = v[6]; st->rsp = v[7];
    st->r8 = v[8]; st->r9 = v[9]; st->r10 = v[10]; st->r11 = v[11];
    st->r12 = v[12]; st->r13 = v[13]; st->r14 = v[14]; st->r15 = v[15];
    uint64_t rip = 0;
    for (int b = 0; b < 8; b++) {
        int h1 = fromhex(*p++), h2 = fromhex(*p++);
        rip |= ((uint64_t)((h1 << 4) | h2)) << (b * 8);
    }
    st->rip = rip;
}

// Set when we resume the target (c/s) so the *next* stop (breakpoint, step, or
// async Ctrl-C) sends GDB the stop reply it is waiting for.
static volatile int g_resumed = 0;

// The RSP command loop. `send_stop` => emit an unsolicited stop reply on entry
// (GDB is waiting for one after a continue/step/Ctrl-C). Returns when GDB
// resumes the target (c or s).
static void gdb_loop(int send_stop) {
    interrupt_register_state_t st;
    interrupt_get_register_state(&st);
    g_resumed = 0;
    if (send_stop)
        send_packet("S05");

    while (1) {
        if (recv_packet() < 0)
            continue;
        char cmd = rx_buf[0];
        const char *args = rx_buf + 1;

        switch (cmd) {
            case '?':  // stop reason
                send_packet("S05");
                break;
            case 'g':  // read registers
                build_g_packet(&st, tx_buf);
                send_packet(tx_buf);
                break;
            case 'G':  // write registers
                parse_G_packet(&st, args);
                interrupt_set_register_state(&st);
                send_packet("OK");
                break;
            case 'm': {  // read memory: m addr,len
                const char *p = args;
                uint64_t addr = parse_hex(&p);
                if (*p == ',') p++;
                uint64_t len = parse_hex(&p);
                if (len > (GDB_BUF - 4) / 2)
                    len = (GDB_BUF - 4) / 2;
                put_hex_bytes(tx_buf, (void *)(uintptr_t)addr, (int)len)[0] = 0;
                send_packet(tx_buf);
                break;
            }
            case 'M': {  // write memory: M addr,len:data
                const char *p = args;
                uint64_t addr = parse_hex(&p);
                if (*p == ',') p++;
                uint64_t len = parse_hex(&p);
                if (*p == ':') p++;
                uint8_t *dst = (uint8_t *)(uintptr_t)addr;
                // Clear CR0.WP so software breakpoints can be written into
                // read-only kernel text (we run with interrupts off, so no
                // preemption can observe WP cleared).
                uint64_t cr0;
                __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
                __asm__ volatile("mov %0, %%cr0" ::"r"(cr0 & ~0x10000ULL));
                for (uint64_t i = 0; i < len; i++) {
                    int h1 = fromhex(*p++), h2 = fromhex(*p++);
                    if (h1 < 0 || h2 < 0) break;
                    dst[i] = (uint8_t)((h1 << 4) | h2);
                }
                __asm__ volatile("mov %0, %%cr0" ::"r"(cr0));
                send_packet("OK");
                break;
            }
            case 'c':  // continue [addr]
                st.rflags &= ~0x100ULL;  // clear TF
                interrupt_set_register_state(&st);
                g_resumed = 1;
                return;
            case 's':  // single step [addr]
                st.rflags |= 0x100ULL;  // set TF
                interrupt_set_register_state(&st);
                g_resumed = 1;
                return;
            case 'H':  // set thread -> OK
                send_packet("OK");
                break;
            case 'D':  // detach
                st.rflags &= ~0x100ULL;
                interrupt_set_register_state(&st);
                send_packet("OK");
                g_resumed = 0;
                return;
            default:
                send_packet("");  // unsupported -> empty (GDB falls back)
                break;
        }
    }
}

// Exception handler (registered for #BP and #DB). g_resumed is true iff we got
// here after a continue/step, in which case GDB is awaiting a stop reply.
static void gdb_exception(int vector) {
    vector = 0;
    gdb_loop(g_resumed);
}

// COM2 UART RX interrupt: GDB sends 0x03 (Ctrl-C) to halt a running target.
// When that byte arrives we break into the stub on the interrupted code's frame
// and always send a stop reply (GDB is waiting for one). Other bytes are
// ignored (GDB only sends 0x03 while the target runs).
static void gdb_com2_rx(int vector) {
    vector = 0;
    if (inb(COM2 + 5) & 0x01) {  // data ready
        int c = inb(COM2);
        if (c == 0x03)
            gdb_loop(1);
    }
}

// Drop into the debugger and wait for GDB (e.g. from a boot script to debug
// early boot). int3 raises #BP -> gdb_exception -> gdb_loop. Returns 0 so it is
// usable as a `CALL:` boot-script target (which checks the return value).
int gdb_stub_wait(void) {
    __asm__ volatile("int3");
    return 0;
}

// Async break-in for transports without a receive IRQ (e.g. USB-serial). The
// transport driver calls this from a polling loop; SysGdb owns the policy: pump
// the transport, and if GDB sent anything (the initial handshake on connect, or
// a lone 0x03 to halt a running target) drop into the stub. The triggering byte
// stays buffered in the transport -- recv_packet discards anything before '$',
// so a stray Ctrl-C is harmless. Returns 0.
int gdb_poll_breakin(void) {
    if (cur_transport.poll != NULL && cur_transport.poll(cur_transport.state) > 0) {
        DEBUG_PRINT("[SysGdb] GDB activity on transport; entering debugger\r\n");
        __asm__ volatile("int3");  // -> gdb_exception -> gdb_loop
    }
    return 0;
}

// Registered in tests.c; gated by test_mode_active(), so this is free on a
// normal boot.
void sysgdb_register_tests(void);

int module_init() {
    com2_init();
    interrupt_register_handler(1, gdb_exception);  // #DB (single-step / hw bp)
    interrupt_register_handler(3, gdb_exception);  // #BP (int3 / sw bp)

    // Async Ctrl-C break-in: route the COM2 UART RX line (ISA IRQ3 -> vector 35)
    // and enable the UART received-data interrupt, so GDB's Ctrl-C halts a
    // running system over COM2 (USB-serial has no IRQ; it uses breakpoints).
    int irq = 3 + 32;
    if (interrupt_allocate(1, interrupt_flags_fixed | interrupt_flags_exclusive, &irq) == 0) {
        interrupt_mapinterrupt(3, irq, false, false);
        interrupt_register_handler(irq, gdb_com2_rx);
        interrupt_setmask(3, false);
        outb(COM2 + 1, 0x01);  // IER: received-data-available interrupt
    }
    DEBUG_PRINT("[SysGdb] GDB stub armed on COM2 (vectors 1,3; async Ctrl-C via IRQ3)\r\n");

    sysgdb_register_tests();
    return 0;
}
