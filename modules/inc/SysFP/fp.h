// Copyright (c) 2018 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_SYSFP_H
#define CARDINAL_SYSFP_H

// Per-core FP init, called on each AP via apscript.txt (cf. vmem_mp_init /
// timer_mp_init). Returns 0 on success.
int fp_mp_init(void);

int fp_platform_getstatesize(void);

int fp_platform_getalign(void);

void fp_platform_getstate(void* buf);

void fp_platform_setstate(const void* buf);

void fp_platform_getdefaultstate(void* buf);

#endif