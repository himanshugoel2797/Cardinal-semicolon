// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

// Host KeySym -> PS/2 set-1 scancode. The fake input driver feeds coreinput the
// exact (key <scancode> <pressed>) tuples the real ps2 driver does (raw set-1
// make codes), so the OS-side keymap and any app that decodes scancodes work
// unchanged under the simulator. We translate from X11 *KeySyms* (not raw X11
// keycodes) so the mapping is independent of the host keyboard layout.
//
// Arrow keys are emitted as their single-byte "gray" set-1 codes (0x48/0x50/
// 0x4B/0x4D) WITHOUT the 0xE0 extended prefix -- the simulator delivers one
// scancode per event and most consumers treat these bare codes as the arrows.

#include "backend.h"

// We avoid including <X11/keysym.h> here so keymap.c builds even without the X11
// headers; the KeySym values below are the stable X11 protocol constants.
#define KS_BackSpace 0xFF08
#define KS_Tab 0xFF09
#define KS_Return 0xFF0D
#define KS_Escape 0xFF1B
#define KS_Left 0xFF51
#define KS_Up 0xFF52
#define KS_Right 0xFF53
#define KS_Down 0xFF54
#define KS_Shift_L 0xFFE1
#define KS_Shift_R 0xFFE2
#define KS_Control_L 0xFFE3
#define KS_Control_R 0xFFE4
#define KS_space 0x020

// Letter scancodes indexed by (c - 'a'), a..z.
static const unsigned char LETTER_SC[26] = {
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,  // a b c d e f g h i
    0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,  // j k l m n o p q r
    0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,        // s t u v w x y z
};

// Digit scancodes for '1'..'9','0' (set-1 puts 0 after 9).
static const unsigned char DIGIT_SC[10] = {
    0x0B,                                                  // '0'
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,  // '1'..'9'
};

int sim_keysym_to_scancode(unsigned long ks) {
    // Latin letters (both cases map to the same physical key).
    if (ks >= 'a' && ks <= 'z') return LETTER_SC[ks - 'a'];
    if (ks >= 'A' && ks <= 'Z') return LETTER_SC[ks - 'A'];
    if (ks >= '0' && ks <= '9') return DIGIT_SC[ks - '0'];

    switch (ks) {
        case KS_space: return 0x39;
        case KS_Return: return 0x1C;
        case KS_Escape: return 0x01;
        case KS_BackSpace: return 0x0E;
        case KS_Tab: return 0x0F;
        case '-': return 0x0C;
        case '=': return 0x0D;
        case '[': return 0x1A;
        case ']': return 0x1B;
        case ';': return 0x27;
        case '\'': return 0x28;
        case '`': return 0x29;
        case '\\': return 0x2B;
        case ',': return 0x33;
        case '.': return 0x34;
        case '/': return 0x35;
        case KS_Shift_L: return 0x2A;
        case KS_Shift_R: return 0x36;
        case KS_Control_L:
        case KS_Control_R: return 0x1D;
        case KS_Up: return 0x48;
        case KS_Down: return 0x50;
        case KS_Left: return 0x4B;
        case KS_Right: return 0x4D;
        default: return -1;
    }
}
