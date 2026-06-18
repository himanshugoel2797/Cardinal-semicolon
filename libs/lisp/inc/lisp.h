// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_LISP_H
#define CARDINAL_LISP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Cardinal; kernel-resident Lisp -- core value representation.
//
// See notes/core/lisp-substrate.md for the why. This is Phase 0: the tagged
// value representation plus reader/printer. The representation is the one
// decision that is painful to retrofit, so it is fixed here deliberately:
//
//   - A `lisp_value` is one machine word. The low 2 bits are a tag:
//       00  heap pointer (objects are >= 8-byte aligned, low 3 bits clear)
//       01  fixnum       (62-bit signed, value = (int64_t)v >> 2)
//       10  immediate    (singletons + char; subtype in bits [7:2])
//       11  reserved      (second immediate space / future use)
//   - Fixnums are unboxed so integer arithmetic never allocates. The kernel is
//     -mno-sse (no floating point), so the numeric tower is integer-centric by
//     design; there are deliberately no float literals.
//   - Every heap object begins with a `lisp_header` word whose layout reserves
//     space for a future GC (mark bits now; forwarding for a moving collector
//     later). v1 is a non-moving mark-sweep; nothing here precludes a nursery.

typedef uintptr_t lisp_value;

#define LISP_TAG_MASK 0x3u
#define LISP_TAG_PTR 0x0u
#define LISP_TAG_FIXNUM 0x1u
#define LISP_TAG_IMM 0x2u

// Immediate subtypes live in bits [7:2]; payload (e.g. a char codepoint) above.
#define LISP_IMM_NIL 0u
#define LISP_IMM_FALSE 1u
#define LISP_IMM_TRUE 2u
#define LISP_IMM_EMPTY 3u  // the empty list (), distinct from nil (Clojure-style)
#define LISP_IMM_CHAR 4u
#define LISP_IMM_EOF 5u    // reader end-of-input sentinel
#define LISP_IMM_UNDEF 6u  // unspecified / void

#define LISP_MK_IMM(subtype, payload) \
    ((lisp_value)(((uintptr_t)(payload) << 8) | ((uintptr_t)(subtype) << 2) | LISP_TAG_IMM))

#define LISP_NIL LISP_MK_IMM(LISP_IMM_NIL, 0)
#define LISP_FALSE LISP_MK_IMM(LISP_IMM_FALSE, 0)
#define LISP_TRUE LISP_MK_IMM(LISP_IMM_TRUE, 0)
#define LISP_EMPTY LISP_MK_IMM(LISP_IMM_EMPTY, 0)
#define LISP_EOF LISP_MK_IMM(LISP_IMM_EOF, 0)
#define LISP_UNDEF LISP_MK_IMM(LISP_IMM_UNDEF, 0)

// Heap object type tags (low byte of the header word).
typedef enum {
    LISP_OBJ_PAIR = 1,
    LISP_OBJ_SYMBOL = 2,
    LISP_OBJ_KEYWORD = 3,
    LISP_OBJ_STRING = 4,
    // reserved for later phases: VECTOR, MAP, CLOSURE, BYTEVECTOR, ...
} lisp_objtype;

// Header bit layout: [7:0] type, [15:8] gc flags, [63:16] aux (type-specific,
// e.g. string/symbol length). A moving collector can later steal the whole word
// for a forwarding pointer (objects it forwards are distinguished by a gc flag).
#define LISP_GC_MARK 0x01u

typedef struct {
    uint64_t header;
} lisp_header;

#define LISP_HDR_TYPE(h) ((lisp_objtype)((h)->header & 0xffu))
#define LISP_HDR_AUX(h) ((h)->header >> 16)
#define LISP_MK_HEADER(type, aux) (((uint64_t)(aux) << 16) | ((uint64_t)(type) & 0xffu))

typedef struct {
    lisp_header h;
    lisp_value car;
    lisp_value cdr;
} lisp_pair;

// Symbols and keywords share a layout; aux holds the name length. (Interning
// comes in a later phase; for now each read produces a fresh object.)
typedef struct {
    lisp_header h;
    uint32_t hash;
    char name[];  // NUL-terminated; length in header aux
} lisp_named;

typedef struct {
    lisp_header h;
    char data[];  // length in header aux (NOT NUL-reliant; may contain NULs)
} lisp_string;

// --- Tag predicates ---------------------------------------------------------

static inline unsigned lisp_tag(lisp_value v) { return (unsigned)(v & LISP_TAG_MASK); }
static inline bool lisp_is_ptr(lisp_value v) { return lisp_tag(v) == LISP_TAG_PTR && v != 0; }
static inline bool lisp_is_fixnum(lisp_value v) { return lisp_tag(v) == LISP_TAG_FIXNUM; }
static inline bool lisp_is_imm(lisp_value v) { return lisp_tag(v) == LISP_TAG_IMM; }
static inline unsigned lisp_imm_subtype(lisp_value v) { return (unsigned)((v >> 2) & 0x3fu); }

static inline bool lisp_is_nil(lisp_value v) { return v == LISP_NIL; }
static inline bool lisp_is_empty(lisp_value v) { return v == LISP_EMPTY; }
static inline bool lisp_is_eof(lisp_value v) { return v == LISP_EOF; }

// Truthiness: only nil and false are falsey (Clojure semantics).
static inline bool lisp_truthy(lisp_value v) { return v != LISP_NIL && v != LISP_FALSE; }

// --- Fixnums ----------------------------------------------------------------

#define LISP_FIXNUM_MAX ((int64_t)(((uint64_t)1 << 61) - 1))
#define LISP_FIXNUM_MIN (-((int64_t)1 << 61))

static inline lisp_value lisp_fixnum(int64_t x) {
    return (lisp_value)(((uintptr_t)x << 2) | LISP_TAG_FIXNUM);
}
static inline int64_t lisp_fixnum_val(lisp_value v) { return (int64_t)v >> 2; }

// --- Char -------------------------------------------------------------------

static inline lisp_value lisp_char(uint32_t cp) { return LISP_MK_IMM(LISP_IMM_CHAR, cp); }
static inline bool lisp_is_char(lisp_value v) {
    return lisp_is_imm(v) && lisp_imm_subtype(v) == LISP_IMM_CHAR;
}
static inline uint32_t lisp_char_val(lisp_value v) { return (uint32_t)(v >> 8); }

// --- Heap object access -----------------------------------------------------

static inline lisp_header *lisp_obj(lisp_value v) { return (lisp_header *)(uintptr_t)v; }
static inline lisp_value lisp_from_obj(void *p) { return (lisp_value)(uintptr_t)p; }

static inline bool lisp_is_objtype(lisp_value v, lisp_objtype t) {
    return lisp_is_ptr(v) && LISP_HDR_TYPE(lisp_obj(v)) == t;
}
static inline bool lisp_is_pair(lisp_value v) { return lisp_is_objtype(v, LISP_OBJ_PAIR); }
static inline bool lisp_is_symbol(lisp_value v) { return lisp_is_objtype(v, LISP_OBJ_SYMBOL); }
static inline bool lisp_is_keyword(lisp_value v) { return lisp_is_objtype(v, LISP_OBJ_KEYWORD); }
static inline bool lisp_is_string(lisp_value v) { return lisp_is_objtype(v, LISP_OBJ_STRING); }

static inline lisp_value lisp_car(lisp_value v) { return ((lisp_pair *)lisp_obj(v))->car; }
static inline lisp_value lisp_cdr(lisp_value v) { return ((lisp_pair *)lisp_obj(v))->cdr; }

// --- Constructors (value.c) -------------------------------------------------

lisp_value lisp_cons(lisp_value car, lisp_value cdr);
lisp_value lisp_make_symbol(const char *name, size_t len);
lisp_value lisp_make_keyword(const char *name, size_t len);
lisp_value lisp_make_string(const char *data, size_t len);

const char *lisp_named_name(lisp_value v);  // symbol/keyword name (NUL-terminated)
size_t lisp_named_len(lisp_value v);
const char *lisp_string_data(lisp_value v);
size_t lisp_string_len(lisp_value v);

// --- Reader (reader.c) ------------------------------------------------------

// Parse one datum from `*cursor`, advancing it past the consumed text. Returns
// LISP_EOF at end of input. On a parse error returns LISP_UNDEF and, if `err`
// is non-NULL, points it at a static message. `end` bounds the buffer.
lisp_value lisp_read(const char **cursor, const char *end, const char **err);

// --- Printer (print.c) ------------------------------------------------------

// Write a canonical (reader-faithful) textual form of `v` into `buf` (capacity
// `cap`). Returns the number of bytes that would be written (excluding the NUL),
// like snprintf, so truncation is detectable. Always NUL-terminates if cap > 0.
size_t lisp_print(lisp_value v, char *buf, size_t cap);

#endif
