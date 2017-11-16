// Copyright (c) 2017 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSMP_PRIV_H
#define CARDINAL_SYSMP_PRIV_H

#include <stdint.h>

int mp_init();

int mp_tls_setup();

int mp_tls_alloc(int bytes);

void* mp_tls_get(int off);

#endif