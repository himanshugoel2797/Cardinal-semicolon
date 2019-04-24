// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_IPC_SYSCALL_LIB_H
#define CARDINAL_IPC_SYSCALL_LIB_H

#include <stdint.h>
#include "error.h"
#include "thread.h"

#define FUNCNAME_LEN (256)
typedef uint64_t obj_t;

typedef enum {
    cs_type_int,
    cs_type_uint,
    cs_type_float,
    cs_type_str,
    cs_type_obj,
} cs_type_t;

typedef enum {
    cs_functype_code,
    cs_functype_inherited,
} cs_functype_t;

typedef struct {
    int len;
    char str[0];
} cs_str_t;

typedef struct {
    union{
        int64_t i;
        uint64_t ui;
        float f;
        obj_t o;
        cs_str_t s;
    };
} cs_arg_t;

typedef struct {
    cs_functype_t type;
    char name[FUNCNAME_LEN];
    union{
        struct {
            uint16_t param_cnt;
            uint16_t ret_cnt;
            void *func;
            cs_type_t* param;
            cs_type_t* ret;
        } code;

        struct {
            obj_t obj;
        } inherited;
    };
} cs_func_t;

cs_error cs_createobj(cs_func_t *func, int func_cnt, obj_t *obj);
cs_error cs_invoke(obj_t obj, char name[256], cs_arg_t *args, cs_arg_t *ret);
cs_error cs_release(obj_t obj);
cs_error cs_periodicinvoke(obj_t obj, char name[256], cs_arg_t *args, uint64_t period_us);
cs_error cs_watchdogtick(void);

#endif