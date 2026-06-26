cmake_minimum_required(VERSION 3.20)

ADD_LIBRARY(ttf STATIC ttf.c)
TARGET_INCLUDE_DIRECTORIES(ttf PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
TARGET_INCLUDE_DIRECTORIES(ttf PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../stb")
TARGET_INCLUDE_DIRECTORIES(ttf SYSTEM PUBLIC "${KERN_STDLIB_INCLUDE_DIR}")

# stb_truetype's rasterizer is float-based, so -- like libs/lisp's flonum runtime,
# and unlike every other kernel module -- this TU re-enables SSE over the kernel's
# blanket -mno-sse. Same recipe as libs/lisp/CMakeLists (see its comment): SSE runs
# only in task context where SysTaskMgr saves/restores FP state; -mstackrealign
# because the -mno-sse call path doesn't guarantee the 16-byte stack SSE spills
# (movaps) need; -fno-vectorize/-fno-slp-vectorize because the module loader doesn't
# 16-align section data, so PACKED aligned .rodata loads (movapd) would #GP -- keep
# it scalar. Only integer/pointer values cross the ttf ABI, so -mno-sse callers that
# link this are unaffected.
# -O2 unconditionally (even in Debug): the stb_truetype rasterizer is hot float code
# and is unusably slow at -O0. Alignment-safe alongside -fno-vectorize (scalar double
# loads are movsd, not the aligned movapd the module loader can't satisfy) -- the
# same combination libs/lisp ships in Release.
TARGET_COMPILE_OPTIONS(ttf PRIVATE -O2 -fno-pic -msse -msse2 -mstackrealign
                                   -fno-vectorize -fno-slp-vectorize -fno-math-errno
                                   -Wno-unused-function -Wno-sign-compare
                                   -Wno-unused-but-set-variable)
TARGET_LINK_LIBRARIES(ttf)
