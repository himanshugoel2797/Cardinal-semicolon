// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSDEBUG_H
#define CARDINAL_SYSDEBUG_H

#include <stdint.h>

typedef int (*DebugFunc) ();

typedef struct {
    char func_name[240];
    DebugFunc func;
} DebugHandler_t;

typedef enum {
    DebugColor_Black,
    DebugColor_White,
    DebugColor_Red,
    DebugColor_Green,
    DebugColor_Blue,
} DebugColor_t;

int register_debughandler(DebugHandler_t *handler);
int unregister_debughandler(DebugHandler_t *handler);

int debug_printstr(const char *str);

int debug_readstr(char *buf, int maxlen);

int debug_setfg(DebugColor_t col);
int debug_setbg(DebugColor_t col);

const char* debug_getlogbase(void);
uint32_t debug_getlogendoffset(void);

#endif