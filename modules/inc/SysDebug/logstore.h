// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT
//
// Per-source debug log store. Each module/server/driver/app logs under its own
// SOURCE name into a small ring buffer; the serial REPL reads them back by name
// (log-sources / log-dump / log-tail / log-clear). This replaces the single
// undifferentiated debug-log ring + the CSMUX log channel: with the REPL owning
// the serial line, component logs live here instead of streaming over the wire.
//
// The store is a fixed table of named rings (bounded memory, no allocation), so
// it is safe to call from any context including a trap. Sources are created on
// first emit; once the table is full, further new names are dropped.

#ifndef CARDINAL_SYSDEBUG_LOGSTORE_H
#define CARDINAL_SYSDEBUG_LOGSTORE_H

#include <stdint.h>

// Append `len` bytes of `msg` to `source`'s ring (creating the source on first
// use). Oldest bytes are overwritten when the ring is full.
void logstore_emit(const char *source, const char *msg, uint32_t len);

// Write a newline-separated list of known source names into `out` (capacity
// `cap`); returns the number of bytes written.
uint32_t logstore_sources(char *out, uint32_t cap);

// Copy `source`'s buffered text (oldest-first) into `out`; returns bytes written
// (0 if the source is unknown).
uint32_t logstore_dump(const char *source, char *out, uint32_t cap);

// Copy the last `nlines` lines of `source` into `out`; returns bytes written.
uint32_t logstore_tail(const char *source, uint32_t nlines, char *out, uint32_t cap);

// Discard `source`'s buffered text (the source name stays known).
void logstore_clear(const char *source);

#endif  // CARDINAL_SYSDEBUG_LOGSTORE_H
