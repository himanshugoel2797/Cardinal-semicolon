#include "serialio.h"
#include "debug_log.h"
#include "elf.h"
#include "boot_information.h"
#include "module_def.h"
#include "initrd.h"
#include "SysDebug/csmux.h"

#include "font.h"
//#include "wallpaper.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SET_BLACK_FG "\e[30m"
#define SET_RED_FG "\e[31m"
#define SET_WHITE_FG "\e[37m"
#define SET_GREEN_FG "\e[32m"

#define SET_BLACK_BG "\e[40m"
#define SET_RED_BG "\e[41m"
#define SET_WHITE_BG "\e[47m"
#define SET_GREEN_BG "\e[42m"

static uint32_t *fbuf = NULL;
static int stride;
static int char_pos = 0;
static int char_pos_limit = 0;
static int line = 0;
static int line_limit = 0;
static char *hex_str = "0123456789ABCDEF";
static char *dec_str = "0123456789";

int kernel_updatetraphandlers();
void print_hexdump(void *datap, int len);
int print_uint64(uint64_t num, uint8_t base);

// Runtime-resolved phys->virt mapper for the debug shell's peek/poke commands.
// SysDebug loads before the VM module so vmem_phystovirt can't be linked, but
// the shell only runs post-boot when it's resolvable via the kernel symbol DB.
static intptr_t (*g_dbg_phystovirt)(intptr_t, uint64_t, int) = NULL;
static volatile void *dbg_map(uint64_t phys)
{
    if (g_dbg_phystovirt == NULL)
        g_dbg_phystovirt = (intptr_t(*)(intptr_t, uint64_t, int))
                           elf_resolvefunction("vmem_phystovirt");
    if (g_dbg_phystovirt == NULL)
        return NULL;
    uint64_t pg = phys & ~0xFFFULL;
    // flags: uncached(1<<5) | kernel(1<<10) | rw(write 1<<0)
    uint8_t *v = (uint8_t *)g_dbg_phystovirt((intptr_t)pg, 4096, 0x20 | 0x400 | 0x1);
    return (volatile void *)(v + (phys & 0xFFF));
}
static uint64_t dbg_hex(const char **s)
{
    const char *p = *s;
    while (*p == ' ') p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    uint64_t v = 0;
    for (;; p++)
    {
        char c = *p;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | (uint64_t)d;
    }
    *s = p;
    return v;
}

// Buffer for a signed .celf module streamed in over serial by the `load`
// command. Lets new debug/experiment code be pushed and run over the bridge
// without reflashing the boot medium.
static uint8_t g_loadbuf[256 * 1024];

// Serial I/O for the interactive PANIC shell, routed through the shared console
// UART (csmux.c) so it follows any ACPI-SPCR retarget instead of assuming the
// legacy 0x3f8 COM1 port.
static void serial_output(char c)
{
    csmux_uart_putb((uint8_t)c);
}

static char serial_input()
{
    int b;
    while ((b = csmux_uart_getb()) < 0)
        ;
    return (char)b;
}

// Read one byte, but give up after `spins` empty polls instead of blocking
// forever -- so a dropped byte mid-transfer can't wedge the receiver.
// csmux_uart_getb is ~1 PIO read/iteration, so a few million spins is a
// sub-second bound: long enough to ride out bridge latency, short enough to
// recover a loss. Returns the byte or -1 on timeout.
#define RECV_SPIN 8000000ULL
static int recv_byte_to(uint64_t spins)
{
    for (uint64_t i = 0; i < spins; i++)
    {
        int b = csmux_uart_getb();
        if (b >= 0)
            return b;
    }
    return -1;
}

// Receive `len` bytes into `buf` over the lossy, flow-control-less serial bridge
// using a chunked + checksummed + acked protocol (the host's send_blob mirrors
// it): for each 512-byte chunk the sender appends a 1-byte sum; we reply 'A' if
// it matches or 'R' to request a resend (also on a read timeout / short chunk).
// A dropped byte then costs one chunk retransmit instead of hanging the whole
// transfer -- which is exactly what the old blind fixed-count read did. Returns
// true when all `len` bytes are in, false if the link gives up.
#define RECV_CHUNK 256   //must match the host send_blob chunk size
static bool recv_blob(uint8_t *buf, uint64_t len, void (*out)(char))
{
    uint64_t recd = 0;
    int fails = 0;
    while (recd < len)
    {
        uint64_t clen = (len - recd < RECV_CHUNK) ? (len - recd) : RECV_CHUNK;
        uint32_t sum = 0;
        bool ok = true;
        for (uint64_t i = 0; i < clen; i++)
        {
            int b = recv_byte_to(RECV_SPIN);
            if (b < 0) { ok = false; break; }
            buf[recd + i] = (uint8_t)b;
            sum += (uint8_t)b;
        }
        int csum = ok ? recv_byte_to(RECV_SPIN) : -1;
        //ACK/NAK are control bytes (0x06/0x15), not printable, so the bridge's
        //injected text beacons on the return path can't be misread as an ack.
        if (ok && csum >= 0 && (uint8_t)sum == (uint8_t)csum)
        {
            recd += clen;
            fails = 0;
            out((char)0x06);
        }
        else
        {
            //Drain stragglers (short timeout) so the resend re-aligns, then NAK.
            while (recv_byte_to(200000) >= 0)
                ;
            if (++fails > 200)
                return false;
            out((char)0x15);
        }
    }
    return true;
}

static void render_char(char c)
{
    int x = 0;
    int y = 0;
    char *bitmap = font8x8_basic[(int)c];
    //memset(fbuf, 0xff, stride * 500);
    for (; x < 8; x++)
    {
        for (y = 0; y < 8; y++)
        {
            bool set = bitmap[y] & 1 << x;
            fbuf[(y + line * 10) * stride / sizeof(uint32_t) + x + 8 * char_pos] = set ? 0xffffffff : 0x0;
        }
    }
    char_pos = (char_pos + 1) % char_pos_limit;
}

int sysdebug_install_lfb()
{

    CardinalBootInfo *b_info = GetBootInfo();
    fbuf = (uint32_t *)(b_info->FramebufferAddress + 0xffff808000000000);
    stride = b_info->FramebufferPitch;
    char_pos_limit = b_info->FramebufferWidth / 8;
    line_limit = b_info->FramebufferHeight / 10;

    /*uint32_t *fbuf_ptr = fbuf;
    char *img_data = header_data;
    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++){
            if (x < b_info->FramebufferWidth && y < b_info->FramebufferHeight){
                uint8_t pixel[3];
                HEADER_PIXEL(img_data, pixel)
                fbuf_ptr[x] = ((uint32_t)pixel[0] << b_info->FramebufferRedFieldPosition) | ((uint32_t)pixel[1] << b_info->FramebufferGreenFieldPosition) | ((uint32_t)pixel[2] << b_info->FramebufferBlueFieldPosition);
            }
        }
        fbuf_ptr += stride / sizeof(uint32_t);
    }*/

        return 0;
}

void print_stream(void (*output_stream)(char) NONNULL,
                  const char *str NONNULL)
{
    while (*str != 0)
    {
        if (fbuf != NULL)
            render_char(*str);
        output_stream(*(str++));
    }
}

// Hook invoked at the top of debug_handle_trap (the PANIC path) before the
// interactive shell. SysTest installs one so a PANIC during a death test reports
// the death and resets instead of hanging in the shell; it returns normally when
// it decides not to act. NULL on a normal boot.
static void (*g_trap_hook)(void) = NULL;
void debug_set_trap_hook(void (*hook)(void)) { g_trap_hook = hook; }

static char priv_s[2048];
int WEAK debug_handle_trap()
{
    if (g_trap_hook != NULL)
        g_trap_hook(); // death-test path: may report + reset (never returns) when armed
    const char *p = priv_s;
    print_str(p);
    debug_shell(serial_input, serial_output);
    return 0;
}
int WEAK print_str(const char *s)
{
    int state = cli();
    //print_stream(serial_output, SET_RED_BG SET_WHITE_FG);
    log(s);
    if (fbuf != NULL)
        for (const char *r = s; *r != 0; r++)
            render_char(*r);
    // Once CSMUX is active, the debug log rides CSMUX_CH_LOG over whatever the one
    // link is -- COM1 or a USB-serial adapter -- so a board whose only serial is a
    // USB-to-serial dongle still gets the log, alongside control (ch1) and GDB
    // (ch2), demuxed by the host. Before activation (or when CSMUX is off) it is
    // raw bytes on COM1, as on a normal boot. csmux_send / csmux_raw_write share
    // the TX lock so raw and framed output never interleave into a corrupt frame,
    // and csmux_send drops a frame re-entered from inside the transport write
    // (a log emitted mid-USB-transfer) rather than deadlocking.
    if (csmux_active())
        csmux_log_append(s, (uint32_t)strlen(s));
    else
        csmux_raw_write(s, (uint32_t)strlen(s));
    //print_stream(serial_output, SET_BLACK_BG SET_WHITE_FG);

    if (fbuf != NULL)
    {

        while (*s != 0)
        {

            if (*(s++) != '\n')
                continue;

            line = (line + 1) % line_limit;

            char_pos = 0;

            if (line == 0){
                memset(fbuf, 0, stride * line_limit * 10);
            }
        }

        //for(int i = 0; i < 50000000; i++)
        //    ;
    }

    sti(state);
    return 0;
}

void WEAK set_trap_str(const char *s)
{
    strncpy(priv_s, s, 2048);
}

int debug_shell(char (*input_stream)(), void (*output_stream)(char))
{

    if (input_stream == NULL)
        return -1;

    if (output_stream == NULL)
        return -1;

    // TODO: make load script be a platform specific file

    const int cmd_buf_len = 1024;
    char cmd_buf[cmd_buf_len];
    int cmd_buf_pos = 0;
    bool clear_buf = false;
    bool cmd_fnd = false;
    memset(cmd_buf, 0, cmd_buf_len);

    print_stream(output_stream,
                 "\r\n" SET_RED_FG
                 "Entering debug shell. Commands: call <fn> | r <phys> [sz] | "
                 "w <phys> <val> [sz] | d <phys>\r\n\r\n" SET_WHITE_FG ">");

    while (true)
    {
        char nchar = input_stream();

        if (nchar == '\e') // Consume esc characters
            continue;

        if (nchar == '\b')
            cmd_buf[--cmd_buf_pos] = 0;
        else if (nchar != '\r')
            cmd_buf[cmd_buf_pos++] = nchar;

        if (nchar == '\b')
        {
            output_stream('\b');
            output_stream(' ');
            output_stream('\b');
        }
        else if (nchar == '\r')
        {
            output_stream('\r');
            output_stream('\n');
        }
        else
            output_stream(nchar);

        // help - list commands
        // call - call method by name
        // clear - clear terminal
        // TODO: add a function to install new commands

        if (nchar == '\r')
        {

            output_stream('\t');

            if (strncmp(cmd_buf, "call", 4) == 0)
            {
                cmd_fnd = true;
                char *func_name = cmd_buf + 5;

                int (*f)() = (int (*)())elf_resolvefunction(func_name);
                if (f == NULL)
                    print_stream(output_stream, "Failed to find the function.\r\n");
                else
                {
                    int retVal = f();
                }
            }

            // Hardware peek/poke over the bridge:
            //   r <phys> [sz]        read sz(1/2/4/8, default 4) bytes
            //   w <phys> <val> [sz]  write
            //   d <phys>             hexdump 64 bytes
            // <phys> is any physical address: PCI config space is the ECAM MMIO,
            // device registers are the BAR MMIO, so the whole bring-up can be
            // driven live without a reflash.
            else if ((cmd_buf[0] == 'r' || cmd_buf[0] == 'w' || cmd_buf[0] == 'd')
                     && cmd_buf[1] == ' ')
            {
                cmd_fnd = true;
                const char *p = cmd_buf + 1;
                uint64_t addr = dbg_hex(&p);
                volatile void *a = dbg_map(addr);
                if (a == NULL)
                    print_stream(output_stream, "map failed\r\n");
                else if (cmd_buf[0] == 'r')
                {
                    uint64_t sz = (*p) ? dbg_hex(&p) : 4;
                    uint64_t val = (sz == 1) ? *(volatile uint8_t *)a
                                 : (sz == 2) ? *(volatile uint16_t *)a
                                 : (sz == 8) ? *(volatile uint64_t *)a
                                             : *(volatile uint32_t *)a;
                    print_stream(output_stream, "= 0x");
                    print_uint64(val, 16);
                    print_stream(output_stream, "\r\n");
                }
                else if (cmd_buf[0] == 'w')
                {
                    uint64_t val = dbg_hex(&p);
                    uint64_t sz = (*p) ? dbg_hex(&p) : 4;
                    if (sz == 1) *(volatile uint8_t *)a = (uint8_t)val;
                    else if (sz == 2) *(volatile uint16_t *)a = (uint16_t)val;
                    else if (sz == 8) *(volatile uint64_t *)a = val;
                    else *(volatile uint32_t *)a = (uint32_t)val;
                    print_stream(output_stream, "ok\r\n");
                }
                else
                {
                    print_hexdump((void *)a, 64);
                }
            }

            // load <hexlen> : receive a signed .celf of <hexlen> bytes raw over
            // serial, then load + relocate it and call its module_init -- so new
            // debug/experiment code can be pushed and run without reflashing.
            else if (strncmp(cmd_buf, "load ", 5) == 0)
            {
                cmd_fnd = true;
                //load <hexlen> [hexarg] : stream a .celf and call module_init(arg).
                //arg is the module_init argument -- e.g. a PCI driver's ECAM
                //address (the RTL8169 needs 0xe0100000). Defaults to 0 for
                //no-arg (Sys-style) modules.
                const char *p = cmd_buf + 5;
                uint64_t len = dbg_hex(&p);
                while (*p == ' ') p++;
                uint64_t arg = (*p) ? dbg_hex(&p) : 0;
                if (len == 0 || len > sizeof(g_loadbuf))
                {
                    print_stream(output_stream, "load: bad length\r\n");
                }
                else
                {
                    //Signal readiness, then receive len bytes (chunked/acked).
                    print_stream(output_stream, "load: send now\r\n");
                    if (!recv_blob(g_loadbuf, len, output_stream))
                    {
                        print_stream(output_stream, "load: rx failed\r\n");
                        clear_buf = true;
                        goto load_done;
                    }

                    ModuleHeader *hdr = (ModuleHeader *)g_loadbuf;
                    if (strncmp(hdr->magic, MODULE_HEADER_MAGIC, 4) != 0)
                    {
                        print_stream(output_stream, "load: bad CELF magic\r\n");
                    }
                    else
                    {
                        int (*entry)() = NULL;
                        if (elf_load(hdr->data, hdr->uncompressed_len, &entry) || entry == NULL)
                            print_stream(output_stream, "load: elf_load failed\r\n");
                        else
                        {
                            print_stream(output_stream, "load: calling module_init\r\n");
                            int r = ((int (*)(void *))entry)((void *)(uintptr_t)arg);
                            print_stream(output_stream, "load: module_init ret=");
                            print_uint64((uint64_t)(int64_t)r, 16);
                            print_stream(output_stream, "\r\n");
                        }
                    }
load_done:;
                }
            }
            // upload <name> <hexlen> : stream <hexlen> raw bytes and register
            // them as an initrd overlay shadowing file <name>. Any boot file --
            // a module .celf, devices.txt, a boot script -- can thus be supplied
            // over serial so the rest of boot uses it, no reflash. Enter the
            // shell before the consumer runs (servicescript CALLs it before
            // CoreDriver), set overlays, then `q` to continue.
            else if (strncmp(cmd_buf, "upload ", 7) == 0)
            {
                cmd_fnd = true;
                const char *p = cmd_buf + 7;
                char name[100];
                int n = 0;
                while (*p == ' ') p++;
                while (*p && *p != ' ' && n < 99) name[n++] = *p++;
                name[n] = 0;
                uint64_t len = dbg_hex(&p);
                if (name[0] == 0 || len == 0 || len > sizeof(g_loadbuf))
                {
                    print_stream(output_stream, "upload: bad name/length\r\n");
                }
                else
                {
                    print_stream(output_stream, "upload: send now\r\n");
                    if (!recv_blob(g_loadbuf, len, output_stream))
                        print_stream(output_stream, "upload: rx failed\r\n");
                    else if (Initrd_AddOverlay(name, g_loadbuf, len) == 0)
                        print_stream(output_stream, "upload: overlay added\r\n");
                    else
                        print_stream(output_stream, "upload: overlay full\r\n");
                }
            }
            // q : leave the debug shell and let boot continue (so overlaid files
            // and serial-loaded drivers take effect once boot resumes).
            else if (cmd_buf[0] == 'q' && (cmd_buf[1] == 0 || cmd_buf[1] == ' '))
            {
                print_stream(output_stream, "continuing boot\r\n");
                return 0;
            }

            if (!cmd_fnd)
                print_stream(
                    output_stream,
                    "Unknown Command. Enter 'help' for a list of commands.\r\n");

            clear_buf = true;
        }

        if ((cmd_buf_pos == cmd_buf_len - 1) | clear_buf)
        {
            memset(cmd_buf, 0, cmd_buf_len);
            clear_buf = false;
            cmd_fnd = false;
            cmd_buf_pos = 0;
            output_stream('>');
        }
    }

    return 0;
}

// Entered from servicescript (CALL:) once boot is otherwise complete. Drops into
// the interactive debug shell over serial iff the kernel was booted with the
// "cardinal.debugshell" cmdline flag -- selected at runtime from the GRUB
// "debug" menu entry over serial, so switching in/out of probe mode needs no
// reflash. A normal boot (no flag) returns immediately.
int sysdebug_shell_if_flagged()
{
    CardinalBootInfo *bi = GetBootInfo();
    if (bi != NULL && strstr(bi->Cmdline, "cardinal.debugshell") != NULL)
        debug_shell(serial_input, serial_output);
    return 0;
}

int init_serial_debug()
{
    // Put the console UART into a known 115200 8N1 state before anything else
    // logs -- the firmware/GRUB hand-off can leave the divisor at the wrong baud
    // on real hardware (e.g. the AtomicPi), garbling all serial output.
    csmux_uart_init();
    kernel_updatetraphandlers();
    print_stream(
        serial_output, SET_GREEN_BG SET_RED_FG
        "\r\nKERNEL DEBUG SERVICES INITIALIZED\r\n" SET_WHITE_FG SET_BLACK_BG);
    return 0;
}

#define BASE_HEX 16

#define PRINT_BITCNT(bitcnt)                       \
    if (base == BASE_HEX)                          \
        for (int i = (bitcnt - 4); i >= 0; i -= 4) \
            csmux_uart_putb(hex_str[(num >> i) & 0xF]);

int WEAK print_int8(int8_t num, uint8_t base)
{
    PRINT_BITCNT(8)
    return 0;
}
int WEAK print_int16(int16_t num, uint8_t base) { PRINT_BITCNT(16) 
    return 0;}
int WEAK print_int32(int32_t num, uint8_t base) { PRINT_BITCNT(32) 
    return 0;}
int WEAK print_int64(int64_t num, uint8_t base) { PRINT_BITCNT(64) 
    return 0;}

int WEAK print_uint8(uint8_t num, uint8_t base) { PRINT_BITCNT(8) 
    return 0;}
int WEAK print_uint16(uint16_t num, uint8_t base) { PRINT_BITCNT(16) 
    return 0;}
int WEAK print_uint32(uint32_t num, uint8_t base) { PRINT_BITCNT(32) 
    return 0;}
int WEAK print_uint64(uint64_t num, uint8_t base) { PRINT_BITCNT(64) 
    return 0;}

void print_hexdump(void *datap, int len)
{
    uint8_t *data = (uint8_t *)datap;
    for (int off = 0; off < len; off += 16)
        for (int l_off = 0; l_off < 16 && off + l_off < len; l_off++)
        {
            if (l_off == 0)
            {
                print_uint64((uint64_t)&data[off + l_off], BASE_HEX);
                print_str("  ");
            }
            print_uint8(data[off + l_off], BASE_HEX);
            if (l_off == 7)
                print_str("  ");
            else if (l_off < 15)
                print_str(" ");
            else
                print_str("\r\n");
        }
    print_str("\r\n");
}