#ifndef CARDINAL_TYPES_H
#define CARDINAL_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#define S(x) #x
#define S_(x) S(x)
#define S__LINE__ S_(__LINE__)

#define LE_ENDIAN 1
#define GE_ENDIAN 2

#include S_(ISA_TYPES_H)
#include S_(PLATFORM_TYPES_H)

#define MAX(a, b)                                                              \
  ({                                                                           \
    __typeof__(a) _a = (a);                                                    \
    __typeof__(b) _b = (b);                                                    \
    _a > _b ? _a : _b;                                                         \
  })

#define MIN(a, b)                                                              \
  ({                                                                           \
    __typeof__(a) _a = (a);                                                    \
    __typeof__(b) _b = (b);                                                    \
    _a < _b ? _a : _b;                                                         \
  })

int debug_handle_trap();

#define KiB(x) (x * 1024ull)
#define MiB(x) (KiB(1) * 1024ull * x)
#define GiB(x) (uint64_t)(MiB(1) * 1024ull * x)
#define TiB(x) (uint64_t)(GiB(1) * 1024ull * x)

#define UNUSED __attribute__((__unused__))
#define NONNULL __attribute__((nonnull))
#define PUBLIC __attribute__((visibility("default")))
#define PRIVATE __attribute__((visibility("hidden")))
#define PURE __attribute__((pure))
#define CONST __attribute__((const))
#define IS_NULL(x)                                                             \
  if (!x)                                                                      \
  debug_handle_trap()

#define SECTION(x) __attribute__((section(x)))
#define PACKED __attribute__((packed))
#define NORETURN __attribute__((noreturn))
#define NAKED __attribute__((naked))
#define NULLABLE
#define NONNULL_RETURN __attribute__((returns_nonnull))
#define WEAK __attribute__((weak))

#define NO_UBSAN __attribute__((no_sanitize("undefined")))

#define SWAP_ENDIAN_32(x)                                                      \
  (((x & 0x000000ff) << 24) | ((x & 0x0000ff00) << 8) |                        \
   ((x & 0x00ff0000) >> 8) | ((x & 0xff000000) >> 24))
#define SWAP_ENDIAN_64(x)                                                      \
  ((SWAP_ENDIAN_32((x >> 32)) << 32) | (SWAP_ENDIAN_32(x & 0xFFFFFFFF)))

#if CUR_ENDIAN == LE_ENDIAN
#define TO_BE_64(x) SWAP_ENDIAN_64(x)
#define TO_BE_32(x) SWAP_ENDIAN_32(x)

#define TO_LE_64(x) (x)
#define TO_LE_32(x) (x)
#else
#define TO_BE_64(x) (x)
#define TO_BE_32(x) (x)

#define TO_LE_64(x) SWAP_ENDIAN_64(x)
#define TO_LE_32(x) SWAP_ENDIAN_32(x)
#endif

int print_str(const char *s);
#define DEBUG_ECHO(msg) print_str(__FILE__ "," S__LINE__ ":" msg "\r\n")
#define DEBUG_PRINT(msg) print_str(msg)

#if !defined(NDEBUG)
// First set the trap message, then raise the trap
void set_trap_str(const char *str);

#define WARN(msg) print_str(__FILE__ "," S__LINE__ ":" msg "\r\n")

#define PANIC(msg)                                                             \
  set_trap_str(__FILE__ "," S__LINE__ ":" msg "\r\n"), debug_handle_trap()

#define ASSERT(x, msg)                                                         \
  if (!(x))                                                                    \
  PANIC(msg)

#else
#define ASSERT(x, msg)
#define PANIC(msg) debug_handle_trap()
#endif /* end of include guard: _OS_TYPES_H_ */

#ifdef __cplusplus
}
#endif

#endif