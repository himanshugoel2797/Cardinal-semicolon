// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSMP_H
#define CARDINAL_SYSMP_H

#include <types.h>

int mp_tls_setup();

int mp_tls_alloc(int bytes);

TLS void* mp_tls_get(int off);

#endif