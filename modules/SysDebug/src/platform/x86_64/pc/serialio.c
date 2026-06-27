#include "serialio.h"
#include "debug_log.h"
#include "elf.h"
#include "boot_information.h"

#include "font.h"
//#include "wallpaper.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <cardinal/local_spinlock.h>

#define SERIAL_A ((uint16_t)0x3f8)
#define SERIAL_A_STATUS (SERIAL_A + 5)

#define SERIAL_B ((uint16_t)0x2f8)
#define SERIAL_B_STATUS (SERIAL_B + 5)

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

static inline bool serial_outputisready()
{
    char s = 0;
    s = inb(SERIAL_A_STATUS);

    return s & 0x20;
}

static inline bool serial_inputisready()
{
    char s = 0;
    s = inb(SERIAL_A_STATUS);

    return s & 0x1;
}

static inline void serial_output(char c)
{
    while (serial_outputisready() == 0)
        ;
    outb(SERIAL_A, c);
}

static inline char serial_input()
{
    char c = 0;
    while (serial_inputisready() == 0)
        ;
    c = inb(SERIAL_A);
    return c;
}

// --- raw COM1 byte transport (post-CSMUX) ------------------------------------
// CSMUX is gone: the debug log and the interactive REPL never share the serial
// line at once, so there is no need to frame it. Before the REPL starts, the log
// streams raw to COM1 (so a normal/CI boot is exactly as before). When the REPL
// claims the line (cardinal.repl), serial_repl_takeover() flips g_serial_log off
// and from then on print_str / Lisp logs go only to the in-memory stores; the
// REPL reads/writes COM1 raw via serial_raw_getb / serial_raw_write.

static int g_serial_lock = 0;          // serialises raw writes across cores
static volatile bool g_serial_log = true;  // stream the log to COM1 until the REPL takes over
static volatile bool g_fb_log = true;  // render the log to the boot framebuffer until the compositor owns it

// Non-blocking: next COM1 byte, or -1 if the RX FIFO is empty.
int serial_raw_getb(void)
{
    if ((inb(SERIAL_A_STATUS) & 0x01) == 0)
        return -1;
    return (int)(uint8_t)inb(SERIAL_A);
}

// Write `len` bytes to COM1, locked so two cores' output cannot interleave.
void serial_raw_write(const void *buf, uint32_t len)
{
    const char *p = (const char *)buf;
    int if_state = cli();
    local_spinlock_lock(&g_serial_lock);
    for (uint32_t i = 0; i < len; i++)
        serial_output(p[i]);
    local_spinlock_unlock(&g_serial_lock);
    sti(if_state);
}

// The REPL claims the serial line: stop streaming the log to it (logs go to the
// in-memory stores, read back via the REPL's log-* prims).
void serial_repl_takeover(void) { g_serial_log = false; }
int serial_log_enabled(void) { return g_serial_log ? 1 : 0; }

// The compositor now owns the scanout: stop rendering the debug log into the boot
// framebuffer so kernel text can't scribble over the composited image. (The log
// still goes to the serial line / the in-memory store; only the on-screen render
// stops.) One-way for the normal path -- the panic shell renders directly and is
// unaffected, so a post-takeover panic still shows on screen.
void sysdebug_fb_log_off(void) { g_fb_log = false; }

// Enable COM1's "received-data-available" interrupt (IER bit 0), so a byte
// arriving on COM1 raises ISA IRQ 4. The Lisp REPL claims that line via the
// generic irq-register/irq-wait bridge so it parks instead of busy-polling for
// input. Call only AFTER the line is routed (a handler is installed for IRQ 4),
// or an early byte would raise an unhandled interrupt. COM1 is otherwise left as
// firmware configured it (8N1); this touches only the interrupt-enable register.
void serial_enable_rx_irq(void)
{
    outb(SERIAL_A + 1, 0x01); // IER: bit0 = Received Data Available interrupt
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

static char priv_s[2048];
int WEAK debug_handle_trap()
{
    const char *p = priv_s;
    print_str(p);
    debug_shell(serial_input, serial_output);
    return 0;
}
int WEAK print_str(const char *s)
{
    int state = cli();
    log(s);
    // Render to the boot framebuffer only until the compositor owns the scanout
    // (sysdebug_fb_log_off): after that, kernel text would scribble over the
    // composited image.
    if (fbuf != NULL && g_fb_log)
        for (const char *r = s; *r != 0; r++)
            render_char(*r);
    // Stream the log raw to COM1 until the REPL claims the line (cardinal.repl).
    // After that, print_str output lives only in the in-memory log (the `log()`
    // ring above + the per-source store) and is read back over the REPL.
    if (g_serial_log)
        serial_raw_write(s, (uint32_t)strlen(s));

    if (fbuf != NULL && g_fb_log)
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
                 "PANIC detected. Entering debug mode.\r\n\r\n" SET_WHITE_FG ">");

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

int init_serial_debug()
{
    kernel_updatetraphandlers();
    print_stream(
        serial_output, SET_GREEN_BG SET_RED_FG
        "\r\nKERNEL DEBUG SERVICES INITIALIZED\r\n" SET_WHITE_FG SET_BLACK_BG);
    return 0;
}

#define BASE_HEX 16

#define PRINT_BITCNT(bitcnt)                           \
    if (base == BASE_HEX)                             \
        for (int i = (bitcnt - 4); i >= 0; i -= 4)     \
        {                                              \
            while (serial_outputisready() == 0)        \
                ;                                      \
            outb(SERIAL_A, hex_str[(num >> i) & 0xF]); \
        }

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