
FILE(GLOB PLATFORM_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/x86_64/pc/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/x86_64/pc/*.S")

SET(PLATFORM_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/inc/platform/x86_64/pc/")
SET(PLATFORM_C_FLAGS "-DMULTIBOOT1")