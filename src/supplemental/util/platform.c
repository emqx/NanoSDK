//
// Copyright 2021 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"
#include "nng/supplemental/util/platform.h"

nng_time
nng_clock(void)
{
	(void) nni_init();
	return (nni_clock());
}

nng_time
nng_timestamp(void)
{
	(void) nni_init();
	return (nni_timestamp());
}

// Sleep for specified msecs.
void
nng_msleep(nng_duration dur)
{
	(void) nni_init();
	nni_msleep(dur);
}

// Create and start a thread.  Note that on some platforms, this might
// actually be a coroutine, with limitations about what system APIs
// you can call.  Therefore, these threads should only be used with the
// I/O APIs provided by nng.  The thread runs until completion.
int
nng_thread_create(nng_thread **thrp, void (*func)(void *), void *arg)
{
	nni_thr *thr;
	int      rv;

	(void) nni_init();

	if ((thr = NNI_ALLOC_STRUCT(thr)) == NULL) {
		return (NNG_ENOMEM);
	}
	*thrp = (void *) thr;
	if ((rv = nni_thr_init(thr, func, arg)) != 0) {
		return (rv);
	}
	nni_thr_run(thr);
	return (0);
}

void
nng_thread_set_name(nng_thread *thr, const char *name)
{
	nni_thr_set_name((void *) thr, name);
}

// Destroy a thread (waiting for it to complete.)  When this function
// returns all resources for the thread are cleaned up.
void
nng_thread_destroy(nng_thread *thr)
{
	nni_thr *t = (void *) thr;
	nni_thr_fini(t);
	NNI_FREE_STRUCT(t);
}

int
nng_getpid()
{
	return nni_plat_getpid();
}

struct nng_mtx {
	nni_mtx m;
};

int
nng_mtx_alloc(nng_mtx **mpp)
{
	nng_mtx *mp;

	(void) nni_init();

	if ((mp = NNI_ALLOC_STRUCT(mp)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_mtx_init(&mp->m);
	*mpp = mp;
	return (0);
}

void
nng_mtx_free(nng_mtx *mp)
{
	if (mp != NULL) {
		nni_mtx_fini(&mp->m);
		NNI_FREE_STRUCT(mp);
	}
}

void
nng_mtx_lock(nng_mtx *mp)
{
	nni_mtx_lock(&mp->m);
}

void
nng_mtx_unlock(nng_mtx *mp)
{
	nni_mtx_unlock(&mp->m);
}

struct nng_cv {
	nni_cv c;
};

int
nng_cv_alloc(nng_cv **cvp, nng_mtx *mx)
{
	nng_cv *cv;

	if ((cv = NNI_ALLOC_STRUCT(cv)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_cv_init(&cv->c, &mx->m);
	*cvp = cv;
	return (0);
}

void
nng_cv_free(nng_cv *cv)
{
	if (cv != NULL) {
		nni_cv_fini(&cv->c);
		NNI_FREE_STRUCT(cv);
	}
}

void
nng_cv_wait(nng_cv *cv)
{
	nni_cv_wait(&cv->c);
}

int
nng_cv_until(nng_cv *cv, nng_time when)
{
	return (nni_cv_until(&cv->c, (nni_time) when));
}

void
nng_cv_wake(nng_cv *cv)
{
	nni_cv_wake(&cv->c);
}

void
nng_cv_wake1(nng_cv *cv)
{
	nni_cv_wake1(&cv->c);
}

uint32_t
nng_random(void)
{
	(void) nni_init();
	return (nni_random());
}

int
nng_atomic_alloc_bool(nng_atomic_bool **b)
{
	if ((*b = nng_alloc(sizeof(nng_atomic_bool))) == NULL) {
		return NNG_ENOMEM;
	}
	nni_atomic_init_bool(*b);

	return (0);
}

void
nng_atomic_free_bool(nng_atomic_bool *b)
{
	nng_free(b, sizeof(nng_atomic_bool));
}

void
nng_atomic_init_bool(nng_atomic_bool *b)
{
	nni_atomic_init_bool(b);
}

void
nng_atomic_set_bool(nng_atomic_bool *b, bool n)
{
	nni_atomic_set_bool(b, n);
}

bool
nng_atomic_get_bool(nng_atomic_bool *b)
{
	return nni_atomic_get_bool(b);
}

bool
nng_atomic_swap_bool(nng_atomic_bool *b, bool n)
{
	return nni_atomic_swap_bool(b, n);
}

int
nng_atomic_alloc64(nng_atomic_u64 **v)
{
	if ((*v = nng_alloc(sizeof(nng_atomic_u64))) == NULL) {
		return NNG_ENOMEM;
	}
	nni_atomic_init64(*v);

	return (0);
}

void
nng_atomic_free64(nng_atomic_u64 *v)
{
	nng_free(v, sizeof(nng_atomic_u64));
}

void
nng_atomic_init64(nng_atomic_u64 *v)
{
	nni_atomic_init64(v);
}

void
nng_atomic_add64(nng_atomic_u64 *v, uint64_t bump)
{
	nni_atomic_add64(v, bump);
}

void
nng_atomic_sub64(nng_atomic_u64 *v, uint64_t bump)
{
	nni_atomic_sub64(v, bump);
}

uint64_t
nng_atomic_get64(nng_atomic_u64 *v)
{
	return nni_atomic_get64(v);
}

void
nng_atomic_set64(nng_atomic_u64 *v, uint64_t u)
{
	nni_atomic_set64(v, u);
}

uint64_t
nng_atomic_swap64(nng_atomic_u64 *v, uint64_t u)
{
	return nni_atomic_swap64(v, u);
}

uint64_t
nng_atomic_dec64_nv(nng_atomic_u64 *v)
{
	return nni_atomic_dec64_nv(v);
}

void
nng_atomic_inc64(nng_atomic_u64 *v)
{
	nni_atomic_inc64(v);
}

bool
nng_atomic_cas64(nng_atomic_u64 *v, uint64_t comp, uint64_t new)
{
	return nni_atomic_cas64(v, comp, new);
}

int
nng_atomic_alloc(nng_atomic_int **v)
{
	if ((*v = nng_alloc(sizeof(nng_atomic_int))) == NULL) {
		return NNG_ENOMEM;
	}
	nni_atomic_init(*v);
	return (0);
}

void
nng_atomic_free(nng_atomic_int *v)
{
	nng_free(v, sizeof(nng_atomic_int));
}

void
nng_atomic_init(nng_atomic_int *v)
{
	nni_atomic_init(v);
}
void
nng_atomic_add(nng_atomic_int *v, int bump)
{
	nni_atomic_add(v, bump);
}

void
nng_atomic_sub(nng_atomic_int *v, int bump)
{
	nni_atomic_sub(v, bump);
}

int
nng_atomic_get(nng_atomic_int *v)
{
	return nni_atomic_get(v);
}

void
nng_atomic_set(nng_atomic_int *v, int i)
{
	nni_atomic_set(v, i);
}

int
nng_atomic_swap(nng_atomic_int *v, int i)
{
	return nni_atomic_swap(v, i);
}

int
nng_atomic_dec_nv(nng_atomic_int *v)
{
	return nni_atomic_dec_nv(v);
}

void
nng_atomic_dec(nng_atomic_int *v)
{
	nni_atomic_dec(v);
}

void
nng_atomic_inc(nng_atomic_int *v)
{
	nni_atomic_inc(v);
}

bool
nng_atomic_cas(nng_atomic_int *v, int comp, int new)
{
	return nni_atomic_cas(v, comp, new);
}

int
nng_atomic_alloc_ptr(nng_atomic_ptr **v)
{
	if ((*v = nng_alloc(sizeof(nng_atomic_ptr))) == NULL) {
		return NNG_ENOMEM;
	}
	return (0);
}

void
nng_atomic_free_ptr(nng_atomic_ptr *v)
{
	nng_free(v, sizeof(nng_atomic_ptr));
}

void
nng_atomic_set_ptr(nng_atomic_ptr *v, void *p)
{
	nni_atomic_set_ptr(v, p);
}

void *
nng_atomic_get_ptr(nng_atomic_ptr *v)
{
	return nni_atomic_get_ptr(v);
}
