// doomgeneric backend for Cardinal; (WASM guest)
//
// The guest is a wasm32-wasi binary; everything outside libc is reached through
// the host-serviced "cardinal" import module (see lisp/lib/wasm-doom.clp):
//   cardinal.present(buf, w, h)   -- blit the 32bpp ARGB frame at linear-memory
//                                    offset `buf` (w*h pixels) to the window.
//   cardinal.poll_key()           -- next input event, or -1 if none. Encoding:
//                                    (pressed << 8) | ps2_scancode  (raw set-1).
//   cardinal.ticks_ms()           -- monotonic milliseconds.
//   cardinal.sleep_ms(ms)         -- yield to the host for ~ms.
// The WAD is read through ordinary stdio (fopen/fseek/fread), which wasi-libc
// lowers to path_open/fd_read/fd_seek against the host's initrd preopen.

#include "doomkeys.h"
#include "doomgeneric.h"

#include <stdint.h>

#define CARDINAL_IMPORT(name) \
  __attribute__((import_module("cardinal"), import_name(#name)))

CARDINAL_IMPORT(present)   void  cardinal_present(const void *buf, int w, int h);
CARDINAL_IMPORT(poll_key)  int   cardinal_poll_key(void);
CARDINAL_IMPORT(ticks_ms)  uint32_t cardinal_ticks_ms(void);
CARDINAL_IMPORT(sleep_ms)  void  cardinal_sleep_ms(uint32_t ms);

// wasi-libc declares system() (POSIX) but provides no implementation; Doom's
// i_system.c error-box path references it. There is no shell in a wasm guest, so
// report "command unavailable".
int system(const char *cmd) {
    (void)cmd;
    return -1;
}

#define KEYQUEUE_SIZE 16
static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

// Map a raw PS/2 set-1 scancode (low 7 bits, release bit already stripped) to a
// Doom key. Mirrors the doomgeneric_soso backend's table.
static unsigned char convertToDoomKey(unsigned char scancode) {
    switch (scancode) {
    case 0x1C: return KEY_ENTER;
    case 0x01: return KEY_ESCAPE;
    case 0x4B: return KEY_LEFTARROW;
    case 0x4D: return KEY_RIGHTARROW;
    case 0x48: return KEY_UPARROW;
    case 0x50: return KEY_DOWNARROW;
    case 0x1D: return KEY_FIRE;     // left ctrl
    case 0x39: return KEY_USE;      // space
    case 0x2A:
    case 0x36: return KEY_RSHIFT;   // shift -> run
    case 0x15: return 'y';
    case 0x21: return 'f';          // f
    default:   return 0;
    }
}

static void addKeyToQueue(int pressed, unsigned char scancode) {
    unsigned char key = convertToDoomKey(scancode);
    unsigned short keyData = (unsigned short)((pressed << 8) | key);
    s_KeyQueue[s_KeyQueueWriteIndex] = keyData;
    s_KeyQueueWriteIndex = (s_KeyQueueWriteIndex + 1) % KEYQUEUE_SIZE;
}

// Drain whatever input the host has queued into our ring.
static void pumpInput(void) {
    for (;;) {
        int ev = cardinal_poll_key();
        if (ev < 0) break;
        int pressed = (ev >> 8) & 1;
        unsigned char sc = (unsigned char)(ev & 0x7F);
        addKeyToQueue(pressed, sc);
    }
}

void DG_Init(void) {
}

void DG_DrawFrame(void) {
    cardinal_present(DG_ScreenBuffer, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    pumpInput();
}

void DG_SleepMs(uint32_t ms) {
    cardinal_sleep_ms(ms);
}

uint32_t DG_GetTicksMs(void) {
    return cardinal_ticks_ms();
}

int DG_GetKey(int *pressed, unsigned char *doomKey) {
    if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex) return 0;
    unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
    s_KeyQueueReadIndex = (s_KeyQueueReadIndex + 1) % KEYQUEUE_SIZE;
    *pressed = keyData >> 8;
    *doomKey = keyData & 0xFF;
    return 1;
}

void DG_SetWindowTitle(const char *title) {
    (void)title;
}

int main(int argc, char **argv) {
    doomgeneric_Create(argc, argv);
    for (;;) {
        doomgeneric_Tick();
    }
    return 0;
}
