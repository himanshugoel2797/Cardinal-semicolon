cmake_minimum_required(VERSION 3.20)

ADD_LIBRARY(ttf STATIC ttf.c)
TARGET_INCLUDE_DIRECTORIES(ttf PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
TARGET_INCLUDE_DIRECTORIES(ttf PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../stb")
TARGET_INCLUDE_DIRECTORIES(ttf SYSTEM PUBLIC "${KERN_STDLIB_INCLUDE_DIR}")

# stb_truetype's rasterizer is float-based, so -- like libs/lisp's flonum runtime,
# and unlike every other kernel module -- this TU re-enables SSE over the kernel's
# blanket -mno-sse. SSE runs only in task context where SysTaskMgr saves/restores FP
# state; -mstackrealign because the -mno-sse call path doesn't guarantee the 16-byte
# stack SSE spills (movaps) need. Only integer/pointer values cross the ttf ABI, so
# the -mno-sse callers that link this are unaffected.
#
# Vectorization IS enabled (SSE2 packed ops): the module loader now lands a module at
# a 64-aligned base (load_script.c) and aligns its .bss to sh_addralign (elf.c), so
# the aligned .rodata.cst16 / packed-constant loads (movapd) that auto-vectorization
# emits are safe. Kept at SSE2 width, not AVX -- some target CPUs (Cherry Trail /
# Airmont, e.g. the AtomicPi) have no AVX; AVX state itself is enabled (SysFP xsetbv).
#
# -O2 unconditionally (even in Debug): the stb_truetype rasterizer is hot float code,
# unusably slow at -O0.
TARGET_COMPILE_OPTIONS(ttf PRIVATE -O2 -fno-pic -msse -msse2 -mstackrealign
                                   -fno-math-errno
                                   -Wno-unused-function -Wno-sign-compare
                                   -Wno-unused-but-set-variable)
TARGET_LINK_LIBRARIES(ttf)
