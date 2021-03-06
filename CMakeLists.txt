CMAKE_MINIMUM_REQUIRED(VERSION 2.8)

FUNCTION(SET_PLATFORM ISA PLATFORM)
INCLUDE("${CMAKE_CURRENT_SOURCE_DIR}/platform/${ISA}/flags.cmake")
INCLUDE("${CMAKE_CURRENT_SOURCE_DIR}/platform/${ISA}/${PLATFORM}/flags.cmake")

SET(CUR_ISA ${ISA} PARENT_SCOPE)
SET(CUR_PLATFORM ${PLATFORM} PARENT_SCOPE)

SET(ISA_SRCS ${ISA_SRCS} PARENT_SCOPE)
SET(PLATFORM_SRCS ${PLATFORM_SRCS} PARENT_SCOPE)
SET(PLATFORM_LAYOUT "${CMAKE_CURRENT_SOURCE_DIR}/platform/${ISA}/${PLATFORM}/linker.ld" PARENT_SCOPE)
SET(PLATFORM_LINKER_FLAGS ${PLATFORM_LINKER_FLAGS} PARENT_SCOPE)
SET(PLATFORM_C_FLAGS ${PLATFORM_C_FLAGS} PARENT_SCOPE)
SET(PLATFORM_ASM_FLAGS ${PLATFORM_ASM_FLAGS} PARENT_SCOPE)
SET(PLATFORM_INCLUDE_DIRS ${PLATFORM_INCLUDE_DIRS} PARENT_SCOPE)
SET(PLATFORM_DEFINITIONS ${PLATFORM_DEFINITIONS} PARENT_SCOPE)
SET(PLATFORM_CELF_DIR ${PLATFORM_CELF_DIR} PARENT_SCOPE)

SET(ISA_LINKER_FLAGS ${ISA_LINKER_FLAGS} PARENT_SCOPE)
SET(ISA_INCLUDE_DIRS ${ISA_INCLUDE_DIRS} PARENT_SCOPE)
SET(ISA_C_FLAGS ${ISA_C_FLAGS} PARENT_SCOPE)
SET(ISA_ASM_FLAGS ${ISA_ASM_FLAGS} PARENT_SCOPE)
SET(ISA_ASM_COMPILER ${ISA_ASM_COMPILER} PARENT_SCOPE)
SET(ISA_LINKER_EXEC ${ISA_LINKER_EXEC} PARENT_SCOPE)
SET(ISA_DEFINITIONS ${ISA_DEFINITIONS} PARENT_SCOPE)

ENDFUNCTION(SET_PLATFORM)

file(STRINGS "KMOD_HMAC_Key.txt" KMOD_HMAC_Key)
file(STRINGS "SERV_HMAC_Key.txt" SERV_HMAC_Key)
STRING(TIMESTAMP CURRENT_YEAR "%Y")
SET(LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs")
SET(LOAD_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/loadscript.txt")
SET(DEVICE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/devices.txt")
SET(AP_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/apscript.txt")
SET(SERVICE_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/servicescript.txt")
SET(CMAKE_USE_RELATIVE_PATHS ON)

SET_PLATFORM("x86_64" "pc")

SET(CELF_GEN "${CMAKE_CURRENT_SOURCE_DIR}/utils_build/sign_exec/sign_exec")
ADD_DEFINITIONS(${ISA_DEFINITIONS} ${PLATFORM_DEFINITIONS} -D${CMAKE_BUILD_TYPE} -D_KERNEL_ -DISA_${CUR_ISA} -DPLATFORM_${CUR_PLATFORM} -DISA="${CUR_ISA}" -DPLATFORM="${CUR_PLATFORM}" -DCURRENT_YEAR="${CURRENT_YEAR}" -DISA_TYPES_H=${CMAKE_CURRENT_SOURCE_DIR}/common/inc/platform/${CUR_ISA}/types.h -DPLATFORM_TYPES_H=${CMAKE_CURRENT_SOURCE_DIR}/common/inc/platform/${CUR_ISA}/${CUR_PLATFORM}/types.h)
SET(CMAKE_C_FLAGS "${ISA_C_FLAGS} ${PLATFORM_C_FLAGS}") 
SET(CMAKE_ASM_COMPILER "${ISA_ASM_COMPILER}")  
SET(CMAKE_ASM_FLAGS "${ISA_ASM_FLAGS} ${PLATFORM_ASM_FLAGS}")
SET(CMAKE_C_LINK_EXECUTABLE "${ISA_LINKER_EXEC} -o <TARGET> <LINK_FLAGS> <OBJECTS> <LINK_LIBRARIES>")
ENABLE_LANGUAGE(ASM)

SET(KERN_STDLIB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/common/inc")
#TARGET_INCLUDE_DIRECTORIES(${CELF_NAME}.celf SYSTEM PUBLIC "${KERN_STDLIB_INCLUDE_DIR}")

#Build common
ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/common") 

#Build libs 
ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/libs/crypto")
ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/libs/miniz")
ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/libs/module_lib")
ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/libs/kvs")
ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/libs/ubsan_handlers")
 

#Build modules
ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/modules")

#Build servers
ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/servers")

#Build drivers
ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/drivers")

#Build kernel
ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/kernel")

#Build Desktop Environment (mana)
ADD_SUBDIRECTORY("${CMAKE_CURRENT_SOURCE_DIR}/mana")