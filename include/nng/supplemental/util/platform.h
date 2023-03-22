//
// Copyright 2018 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef NNG_SUPPLEMENTAL_UTIL_PLATFORM_H
#define NNG_SUPPLEMENTAL_UTIL_PLATFORM_H

// The declarations in this file are provided to assist with application
// portability.  Conceptually these APIs are based on work we have already
// done for NNG internals, and we find that they are useful in building
// portable applications.

// If it is more natural to use native system APIs like pthreads or C11
// APIs or Windows APIs, then by all means please feel free to simply
// ignore this.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// nng_time represents an absolute time since some arbitrary point in the
// past, measured in milliseconds.  The values are always positive.
typedef uint64_t nng_time;

// Return an absolute time from some arbitrary point.  The value is
// provided in milliseconds, and is of limited resolution based on the
// system clock.  (Do not use it for fine grained performance measurements.)
NNG_DECL nng_time nng_clock(void);

// Return unix timestamp (milliseconds) .
NNG_DECL nng_time nng_timestamp(void);

// Sleep for specified msecs.
NNG_DECL void nng_msleep(nng_duration);

// nng_thread is a handle to a "thread", which may be a real system
// thread, or a coroutine on some platforms.
typedef struct nng_thread nng_thread;

// Create and start a thread.  Note that on some platforms, this might
// actually be a coroutine, with limitations about what system APIs
// you can call.  Therefore, these threads should only be used with the
// I/O APIs provided by nng.  The thread runs until completion.
NNG_DECL int nng_thread_create(nng_thread **, void (*)(void *), void *);

// Set the thread name.  Support for this is platform specific and varies.
// It is intended to provide information for use when debugging applications,
// and not for programmatic use beyond that.
NNG_DECL void nng_thread_set_name(nng_thread *, const char *);

// Destroy a thread (waiting for it to complete.)  When this function
// returns all resources for the thread are cleaned up.
NNG_DECL void nng_thread_destroy(nng_thread *);

// Get current process Id.
NNG_DECL int nng_getpid();

// nng_mtx represents a mutex, which is a simple, non-reentrant, boolean lock.
typedef struct nng_mtx nng_mtx;

// nng_mtx_alloc allocates a mutex structure.
NNG_DECL int nng_mtx_alloc(nng_mtx **);

// nng_mtx_free frees the mutex.  It must not be locked.
NNG_DECL void nng_mtx_free(nng_mtx *);

// nng_mtx_lock locks the mutex; if it is already locked it will block
// until it can be locked.  If the caller already holds the lock, the
// results are undefined (a panic may occur).
NNG_DECL void nng_mtx_lock(nng_mtx *);

// nng_mtx_unlock unlocks a previously locked mutex.  It is an error to
// call this on a mutex which is not owned by caller.
NNG_DECL void nng_mtx_unlock(nng_mtx *);

// nng_cv is a condition variable.  It is always allocated with an
// associated mutex, which must be held when waiting for it, or
// when signaling it.
typedef struct nng_cv nng_cv;

NNG_DECL int nng_cv_alloc(nng_cv **, nng_mtx *);

// nng_cv_free frees the condition variable.
NNG_DECL void nng_cv_free(nng_cv *);

// nng_cv_wait waits until the condition variable is "signaled".
NNG_DECL void nng_cv_wait(nng_cv *);

// nng_cv_until waits until either the condition is signaled, or
// the timeout expires.  It returns NNG_ETIMEDOUT in that case.
NNG_DECL int nng_cv_until(nng_cv *, nng_time);

// nng_cv_wake wakes all threads waiting on the condition.
NNG_DECL void nng_cv_wake(nng_cv *);

// nng_cv_wake1 wakes only one thread waiting on the condition.  This may
// reduce the thundering herd problem, but care must be taken to ensure
// that no waiter starves forever.
NNG_DECL void nng_cv_wake1(nng_cv *);

// nng_random returns a "strong" (cryptographic sense) random number.
NNG_DECL uint32_t nng_random(void);

//
// Atomics support.  This will evolve over time.
//

// nng_atomic_flag supports only test-and-set and reset operations.
// This can be implemented without locks on any reasonable system, and
// it corresponds to C11 atomic flag.
typedef struct nni_atomic_flag nng_atomic_flag;

NNG_DECL bool nng_atomic_flag_test_and_set(nng_atomic_flag *);
NNG_DECL void nng_atomic_flag_reset(nng_atomic_flag *);

// nng_atomic_bool is for boolean flags that need to be checked without
// changing their value.  This might require a lock on some systems.
typedef struct nni_atomic_bool nng_atomic_bool;

NNG_DECL int  nng_atomic_alloc_bool(nng_atomic_bool **v);
NNG_DECL void nng_atomic_free_bool(nng_atomic_bool *v);
NNG_DECL void nng_atomic_set_bool(nng_atomic_bool *, bool);
NNG_DECL bool nng_atomic_get_bool(nng_atomic_bool *);
NNG_DECL bool nng_atomic_swap_bool(nng_atomic_bool *, bool);

typedef struct nni_atomic_u64 nng_atomic_u64;

NNG_DECL int      nng_atomic_alloc64(nng_atomic_u64 **v);
NNG_DECL void     nng_atomic_free64(nng_atomic_u64 *v);
NNG_DECL void     nng_atomic_add64(nng_atomic_u64 *, uint64_t);
NNG_DECL void     nng_atomic_sub64(nng_atomic_u64 *, uint64_t);
NNG_DECL uint64_t nng_atomic_get64(nng_atomic_u64 *);
NNG_DECL void     nng_atomic_set64(nng_atomic_u64 *, uint64_t);
NNG_DECL uint64_t nng_atomic_swap64(nng_atomic_u64 *, uint64_t);
NNG_DECL uint64_t nng_atomic_dec64_nv(nng_atomic_u64 *);
NNG_DECL void     nng_atomic_inc64(nng_atomic_u64 *);

// nng_atomic_cas64 is a compare and swap.  The second argument is the
// value to compare against, and the third is the new value. Returns
// true if the value was set.
NNG_DECL bool nng_atomic_cas64(nng_atomic_u64 *, uint64_t, uint64_t);

// In a lot of circumstances, we want a simple atomic reference count,
// or atomic tunable values for integers like queue lengths or TTLs.
// These native integer forms should be preferred over the 64 bit versions
// unless larger bit sizes are truly needed.  They will be more efficient
// on many platforms.
typedef struct nni_atomic_int nng_atomic_int;

NNG_DECL int  nng_atomic_alloc(nng_atomic_int **v);
NNG_DECL void nng_atomic_free(nng_atomic_int *v);
NNG_DECL void nng_atomic_add(nng_atomic_int *, int);
NNG_DECL void nng_atomic_sub(nng_atomic_int *, int);
NNG_DECL int  nng_atomic_get(nng_atomic_int *);
NNG_DECL void nng_atomic_set(nng_atomic_int *, int);
NNG_DECL int  nng_atomic_swap(nng_atomic_int *, int);
NNG_DECL int  nng_atomic_dec_nv(nng_atomic_int *);
NNG_DECL void nng_atomic_dec(nng_atomic_int *);
NNG_DECL void nng_atomic_inc(nng_atomic_int *);

// nng_atomic_cas is a compare and swap.  The second argument is the
// value to compare against, and the third is the new value. Returns
// true if the value was set.
NNG_DECL bool nng_atomic_cas(nng_atomic_int *, int, int);

// atomic pointers.  We only support a few operations.
typedef struct nni_atomic_ptr nng_atomic_ptr;
NNG_DECL int                  nng_atomic_alloc_ptr(nng_atomic_ptr **v);
NNG_DECL void                 nng_atomic_free_ptr(nng_atomic_ptr *v);
NNG_DECL void                 nng_atomic_set_ptr(nng_atomic_ptr *, void *);
NNG_DECL void *               nng_atomic_get_ptr(nng_atomic_ptr *);

#ifdef __cplusplus
}
#endif

#endif // NNG_SUPPLEMENTAL_UTIL_PLATFORM_H
