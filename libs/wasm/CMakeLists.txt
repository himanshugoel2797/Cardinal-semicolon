cmake_minimum_required(VERSION 3.20)

# Cardinal; WebAssembly guest interpreter. Lisp-agnostic, freestanding-capable C.
# Linked into modules/SysLisp (phase 3) the same way libs/lisp and libs/ttf are;
# also host-compiled by the differential test harness (libs/wasm/test).

FILE(GLOB WASM_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

ADD_LIBRARY(wasm STATIC ${WASM_SRCS})
TARGET_INCLUDE_DIRECTORIES(wasm PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/inc")
TARGET_INCLUDE_DIRECTORIES(wasm PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
TARGET_INCLUDE_DIRECTORIES(wasm SYSTEM PUBLIC "${KERN_STDLIB_INCLUDE_DIR}")

# f32/f64 opcodes use hardware doubles, so re-enable SSE locally (the kernel
# builds with -mno-sse globally; libs/lisp and libs/ttf do the same).
TARGET_COMPILE_OPTIONS(wasm PRIVATE -O2 -fno-pic -msse -msse2 -mstackrealign
                                    -Wno-unused-function)
TARGET_LINK_LIBRARIES(wasm)
