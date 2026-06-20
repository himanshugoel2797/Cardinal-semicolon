#ifndef _STDINT_H_
#define _STDINT_H_

#undef NEED_SIZE_T_ONLY
#undef CARDINAL_TYPES_H
#include "types.h"

// The standard <stdint.h> limit macros. types.h supplies only the typedefs, so
// these were absent in the freestanding build; code that uses UINT32_MAX & co.
// (e.g. libs/lisp_shader) needs them. Values are the x86_64 LP64 definitions.
#ifndef UINT8_MAX
#define INT8_MIN (-0x7f - 1)
#define INT16_MIN (-0x7fff - 1)
#define INT32_MIN (-0x7fffffff - 1)
#define INT64_MIN (-0x7fffffffffffffffLL - 1)
#define INT8_MAX 0x7f
#define INT16_MAX 0x7fff
#define INT32_MAX 0x7fffffff
#define INT64_MAX 0x7fffffffffffffffLL
#define UINT8_MAX 0xff
#define UINT16_MAX 0xffff
#define UINT32_MAX 0xffffffffU
#define UINT64_MAX 0xffffffffffffffffULL
#endif
#ifndef SIZE_MAX
#define SIZE_MAX UINT64_MAX
#endif

#endif