// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSDEBUG_H
#define CARDINAL_SYSDEBUG_H

#include <stdint.h>

// NOTE: serial/console output is emitted via print_str() / the DEBUG_PRINT
// macro (see common/inc/types.h), not through this header. The only public
// SysDebug API is the in-memory debug-log accessor pair below.

const char* debug_getlogbase(void);
uint32_t debug_getlogendoffset(void);

#endif