#ifndef _STDINT_H_
#define _STDINT_H_

#undef NEED_SIZE_T_ONLY
#undef CARDINAL_TYPES_H
#include "types.h"

// Fixed-width integer limit macros. The freestanding kernel build resolves
// <stdint.h> here (via -nostdinc + the common/ system include); host builds use
// the toolchain header instead, so these guarded definitions never clash.
#ifndef INT8_MAX
#define INT8_MIN   (-128)
#define INT8_MAX   (127)
#define UINT8_MAX  (255)
#define INT16_MIN  (-32768)
#define INT16_MAX  (32767)
#define UINT16_MAX (65535)
#define INT32_MIN  (-2147483647 - 1)
#define INT32_MAX  (2147483647)
#define UINT32_MAX (4294967295U)
#define INT64_MIN  (-9223372036854775807LL - 1)
#define INT64_MAX  (9223372036854775807LL)
#define UINT64_MAX (18446744073709551615ULL)
#endif

#endif