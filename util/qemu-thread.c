/*
 * Utility functions for mutexes
 *
 * Copyright Red Hat, Inc. 2015
 *
 * Author:
 *   Paolo Bonzini <pbonzini@redhat.com>
 */
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/time.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <linux/futex.h>
#endif
#include "qemu/thread.h"
#include "qemu/atomic.h"

void atomic_inc_with_qemu_mutex(int *x, QemuMutex *mutex)
{
    int old;
    for (;;) {
        old = atomic_mb_read(x);
        if (old == 0) {
            qemu_mutex_lock(mutex);
            atomic_inc(x);
            qemu_mutex_unlock(mutex);
            return;
        } else {
            if (atomic_cmpxchg(x, old, old + 1) == old) {
                return;
            }
        }
    }
}

/* Decrement a counter, and return locked if it is decremented to zero.
 * Used together with atomic_inc_with_qemu_mutex, it is impossible
 * for the counter to become nonzero while the mutex is taken.
 */
bool atomic_dec_and_qemu_mutex_lock(int *x, QemuMutex *mutex)
{
    int old;
    for (;;) {
        old = atomic_mb_read(x);
        if (old == 1) {
            qemu_mutex_lock(mutex);
            if (atomic_fetch_dec(x) == 1) {
                return true;
            }
            qemu_mutex_unlock(mutex);
            return false;
        } else {
            if (atomic_cmpxchg(x, old, old - 1) == old) {
                return false;
            }
        }
    }
}
