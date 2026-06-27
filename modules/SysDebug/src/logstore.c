// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
//
// Per-source debug log store (see SysDebug/logstore.h). A fixed table of named
// ring buffers; emit appends, the REPL reads back by name. All access is under a
// cli()+spinlock so a log from any context (including a trap) is safe.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <types.h>
#include <cardinal/local_spinlock.h>

#include "SysDebug/logstore.h"

#define LOGSTORE_MAX_SOURCES 32
#define LOGSTORE_NAME_MAX 24
#define LOGSTORE_RING 2048u  // per-source ring capacity (recent log tail)

typedef struct {
    char name[LOGSTORE_NAME_MAX];
    char buf[LOGSTORE_RING];
    uint32_t head;  // next write position (mod LOGSTORE_RING)
    uint32_t len;   // valid bytes in the ring (<= LOGSTORE_RING)
} logsource_t;

static logsource_t g_src[LOGSTORE_MAX_SOURCES];
static int g_nsrc = 0;
static int g_lock = 0;

static int name_eq(const char *a, const char *b, uint32_t blen) {
    for (uint32_t i = 0; i < blen; i++)
        if (a[i] != b[i])
            return 0;
    return a[blen] == '\0';  // a must end exactly where b does
}

static uint32_t cstrlen(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

// Find a source by name, or NULL. Caller holds the lock.
static logsource_t *find(const char *name, uint32_t nlen) {
    for (int i = 0; i < g_nsrc; i++)
        if (name_eq(g_src[i].name, name, nlen))
            return &g_src[i];
    return NULL;
}

// Find or create. NULL only when the table is full. Caller holds the lock.
static logsource_t *find_or_add(const char *name, uint32_t nlen) {
    logsource_t *s = find(name, nlen);
    if (s != NULL)
        return s;
    if (g_nsrc >= LOGSTORE_MAX_SOURCES)
        return NULL;
    if (nlen >= LOGSTORE_NAME_MAX)
        nlen = LOGSTORE_NAME_MAX - 1;
    s = &g_src[g_nsrc++];
    memcpy(s->name, name, nlen);
    s->name[nlen] = '\0';
    s->head = 0;
    s->len = 0;
    return s;
}

void logstore_emit(const char *source, const char *msg, uint32_t len) {
    if (source == NULL || msg == NULL)
        return;
    uint32_t nlen = cstrlen(source);
    int if_state = cli();
    local_spinlock_lock(&g_lock);
    logsource_t *s = find_or_add(source, nlen);
    if (s != NULL) {
        for (uint32_t i = 0; i < len; i++) {
            s->buf[s->head] = msg[i];
            s->head = (s->head + 1) % LOGSTORE_RING;
            if (s->len < LOGSTORE_RING)
                s->len++;
        }
    }
    local_spinlock_unlock(&g_lock);
    sti(if_state);
}

uint32_t logstore_sources(char *out, uint32_t cap) {
    if (out == NULL || cap == 0)
        return 0;
    uint32_t o = 0;
    int if_state = cli();
    local_spinlock_lock(&g_lock);
    for (int i = 0; i < g_nsrc; i++) {
        for (const char *p = g_src[i].name; *p != '\0' && o < cap; p++)
            out[o++] = *p;
        if (o < cap)
            out[o++] = '\n';
    }
    local_spinlock_unlock(&g_lock);
    sti(if_state);
    return o;
}

// Copy a source's ring oldest-first into out. Caller holds the lock.
static uint32_t dump_locked(logsource_t *s, char *out, uint32_t cap) {
    uint32_t n = s->len < cap ? s->len : cap;
    uint32_t start = (s->head + LOGSTORE_RING - s->len) % LOGSTORE_RING;
    for (uint32_t i = 0; i < n; i++)
        out[i] = s->buf[(start + i) % LOGSTORE_RING];
    return n;
}

uint32_t logstore_dump(const char *source, char *out, uint32_t cap) {
    if (source == NULL || out == NULL || cap == 0)
        return 0;
    uint32_t nlen = cstrlen(source);
    uint32_t n = 0;
    int if_state = cli();
    local_spinlock_lock(&g_lock);
    logsource_t *s = find(source, nlen);
    if (s != NULL)
        n = dump_locked(s, out, cap);
    local_spinlock_unlock(&g_lock);
    sti(if_state);
    return n;
}

uint32_t logstore_tail(const char *source, uint32_t nlines, char *out,
                       uint32_t cap) {
    if (source == NULL || out == NULL || cap == 0)
        return 0;
    uint32_t nlen = cstrlen(source);
    uint32_t n = 0;
    int if_state = cli();
    local_spinlock_lock(&g_lock);
    logsource_t *s = find(source, nlen);
    if (s != NULL) {
        n = dump_locked(s, out, cap);
        // Trim to the last nlines: walk back over newlines from the end.
        if (nlines > 0 && n > 0) {
            uint32_t seen = 0;
            uint32_t i = n;  // one past the last byte
            // The final byte is usually a trailing newline of the last line; skip it.
            uint32_t scan = (out[n - 1] == '\n') ? n - 1 : n;
            while (i > 0) {
                if (i <= scan && out[i - 1] == '\n') {
                    seen++;
                    if (seen == nlines)
                        break;
                }
                i--;
            }
            if (i > 0) {  // shift the kept tail to the front
                uint32_t keep = n - i;
                for (uint32_t k = 0; k < keep; k++)
                    out[k] = out[i + k];
                n = keep;
            }
        }
    }
    local_spinlock_unlock(&g_lock);
    sti(if_state);
    return n;
}

void logstore_clear(const char *source) {
    if (source == NULL)
        return;
    uint32_t nlen = cstrlen(source);
    int if_state = cli();
    local_spinlock_lock(&g_lock);
    logsource_t *s = find(source, nlen);
    if (s != NULL) {
        s->head = 0;
        s->len = 0;
    }
    local_spinlock_unlock(&g_lock);
    sti(if_state);
}
