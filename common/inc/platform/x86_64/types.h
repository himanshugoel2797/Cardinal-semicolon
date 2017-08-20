#ifndef CARDINAL_x86_64_TYPES_H
#define CARDINAL_x86_64_TYPES_H

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long int uint64_t;

typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef signed long int int64_t;

typedef long int off_t;
typedef long int size_t;

typedef long int intptr_t;
typedef unsigned long int uintptr_t;

typedef int bool;

#define true 1
#define false 0

#define CUR_ENDIAN LE_ENDIAN

#endif