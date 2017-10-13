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

#define KiB(x) (x * 1024ull)
#define MiB(x) (KiB(1) * 1024ull * x)
#define GiB(x) (uint64_t)(MiB(1) * 1024ull * x)

#define UNUSED(x) UNUSED_##x __attribute__((__unused__))
#define NONNULL(...) __attribute__((nonnull(__VA_ARGS__)))
#define PURE __attribute__((pure))
#define IS_NULL(x)                                                             \
  if (!x)                                                                      \
  __builtin_trap()

#define SECTION(x) __attribute__((section(x)))
#define PACKED __attribute__((packed))
#define NORETURN __attribute__((noreturn))
#define NONNULL_RETURN __attribute__((returns_nonnull))
#define WEAK __attribute__((weak))

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

#define PANIC(msg)                                                             \
  set_trap_str(__FILE__ "," S__LINE__ ":" msg), __builtin_trap()

#if defined(DEBUG)
// First set the trap message, then raise the trap
void set_trap_str(const char *str);
#define ASSERT(x, msg)                                                         \
  if (!(x))                                                                    \
  PANIC(msg)
#else
#define ASSERT(x, msg)
#endif /* end of include guard: _OS_TYPES_H_ */

#ifdef __cplusplus
}
#endif

#endif