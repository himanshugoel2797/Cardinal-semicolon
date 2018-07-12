#include "serialio.h"
#include "elf.h"
#include "boot_information.h"

#include "font.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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

int kernel_updatetraphandlers();

static inline bool serial_outputisready() {
    char s = 0;
    s = inb(SERIAL_A_STATUS);

    return s & 0x20;
}

static inline bool serial_inputisready() {
    char s = 0;
    s = inb(SERIAL_A_STATUS);

    return s & 0x1;
}

static inline void serial_output(char c) {
    while (serial_outputisready() == 0)
        ;
    outb(SERIAL_A, c);
}

static inline char serial_input() {
    char c = 0;
    while (serial_inputisready() == 0)
        ;
    c = inb(SERIAL_A);
    return c;
}

static void render_char(char c) {
    int x = 0;
    int y = 0;
    char *bitmap = font8x8_basic[(int)c];
    //memset(fbuf, 0xff, stride * 500);
    for (; x < 8; x++) {
        for (y=0; y < 8; y++) {
            bool set = bitmap[y] & 1 << x;
            fbuf[ (y + line * 10) * stride / sizeof(uint32_t) + x + 8 * char_pos] = set ? 0xffffffff : 0x0;
        }
    }
    char_pos = (char_pos + 1) % char_pos_limit;
}

int sysdebug_install_lfb(){

    CardinalBootInfo *b_info = GetBootInfo();
    fbuf = (uint32_t*)(b_info->FramebufferAddress + 0xffff808000000000);
    stride = b_info->FramebufferPitch;
    char_pos_limit = b_info->FramebufferWidth / 8;
    line_limit = b_info->FramebufferHeight / 10;

    return 0;
}

void print_stream(void (*output_stream)(char) NONNULL,
                  const char *str NONNULL) {
    while (*str != 0){
        if(fbuf != NULL)
            render_char(*str);
        output_stream(*(str++));
    }
}

static char priv_s[2048];
int WEAK debug_handle_trap() {
    const char *p = priv_s;
    print_str(p);
    debug_shell(serial_input, serial_output);
    return 0;
}
int WEAK print_str(const char *s) {
    int state = cli();
    //print_stream(serial_output, SET_RED_BG SET_WHITE_FG);
    print_stream(serial_output, s);
    //print_stream(serial_output, SET_BLACK_BG SET_WHITE_FG);
    
    if(fbuf != NULL){
        line = (line + 1) % line_limit;
        
        char_pos = 0;

        if(line == 0)
            memset(fbuf, 0, stride * line_limit * 10);

        for(int i = 0; i < 5000000; i++)
            ;
    }

    sti(state);
    return 0;
}

void WEAK set_trap_str(const char *s) {
    strncpy(priv_s, s, 2048);
}

int debug_shell(char (*input_stream)(), void (*output_stream)(char)) {

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

    while (true) {
        char nchar = input_stream();

        if (nchar == '\e') // Consume esc characters
            continue;

        if (nchar == '\b')
            cmd_buf[--cmd_buf_pos] = 0;
        else if (nchar != '\r')
            cmd_buf[cmd_buf_pos++] = nchar;

        if (nchar == '\b') {
            output_stream('\b');
            output_stream(' ');
            output_stream('\b');
        } else if (nchar == '\r') {
            output_stream('\r');
            output_stream('\n');
        } else
            output_stream(nchar);

        // help - list commands
        // call - call method by name
        // clear - clear terminal
        // TODO: add a function to install new commands

        if (nchar == '\r') {

            output_stream('\t');

            if (strncmp(cmd_buf, "call", 4) == 0) {
                cmd_fnd = true;
                char *func_name = cmd_buf + 5;

                int (*f)() = (int (*)())elf_resolvefunction(func_name);
                if (f == NULL)
                    print_stream(output_stream, "Failed to find the function.\r\n");
                else {
                    int retVal = f();
                }
            }

            if (!cmd_fnd)
                print_stream(
                    output_stream,
                    "Unknown Command. Enter 'help' for a list of commands.\r\n");

            clear_buf = true;
        }

        if ((cmd_buf_pos == cmd_buf_len - 1) | clear_buf) {
            memset(cmd_buf, 0, cmd_buf_len);
            clear_buf = false;
            cmd_fnd = false;
            cmd_buf_pos = 0;
            output_stream('>');
        }
    }

    return 0;
}

int init_serial_debug() {
    kernel_updatetraphandlers();
    print_stream(
        serial_output, SET_GREEN_BG SET_RED_FG
        "\r\nKERNEL DEBUG SERVICES INITIALIZED\r\n" SET_WHITE_FG SET_BLACK_BG);
    return 0;
}