// Copyright (c) 2017 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef LIBCARDINAL_LOCAL_SPINLOCK_H
#define LIBCARDINAL_LOCAL_SPINLOCK_H

#include <types.h>

static inline void local_spinlock_lock(int *x) {
    while (__sync_lock_test_and_set(x, 1)) {
        while (*x)
            __asm__ volatile("pause");
    }
}

static inline bool local_spinlock_trylock(int *x) {
    return !__sync_lock_test_and_set(x, 1);
}

static inline void local_spinlock_unlock(int *x) {
    __sync_lock_release(x);
}

#endif