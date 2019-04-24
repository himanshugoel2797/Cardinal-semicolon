/**
 * Copyright (c) 2019 himanshu
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include <stdint.h>
#include <stddef.h>

#define DEBUG_LOG_LEN (KiB(32))

static char debug_log[DEBUG_LOG_LEN];
static uint32_t write_cursor;

void log(const char *str) {
    const char* cursor = str;
    while(*cursor != '\0') {
        debug_log[write_cursor] = *cursor;

        write_cursor = (write_cursor + 1) % DEBUG_LOG_LEN;
        cursor++;
    }
}

const char* debug_getlogbase(void) {
    return debug_log;
}

uint32_t debug_getlogendoffset(void) {
    return write_cursor;
}