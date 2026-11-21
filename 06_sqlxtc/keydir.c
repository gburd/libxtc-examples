/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/keydir.c
 *	The keydir.  See keydir.h.
 *
 *	Resident map: KD_STRIPES stripes, each an open-addressing table
 *	with linear probing and backward-shift deletion (no tombstones), a
 *	sequence count (odd while a writer changes the stripe) for the
 *	lock-free readers, and a change counter that commits and drops
 *	bump.  A slot is two 64-bit words, (tableid << 32 | frame) and the
 *	rowid; frame 0 marks an empty slot.  The writer lock is a pthread
 *	mutex held for a few instructions inside a proc critical section,
 *	as bufmgr's stripes and the WAL's locks are (it never parks, so a
 *	parking lock would only add cost).
 *
 *	Delta: per table an xtc_cskip ordered by rowid, DESCENDING, so that
 *	xtc_cskip_floor answers "the smallest rowid >= r" (cskip has no
 *	successor query); the tables are a cskip of the same shape.  The
 *	writers of the active delta hold an xtc_arwlock shared, and a
 *	freeze takes it exclusive, so no commit lands in a frozen delta.
 */

#include "keydir.h"

#include "xtc.h"
#include "xtc_atomic.h"
#include "xtc_cskip.h"
#include "xtc_proc.h"
#include "xtc_rcu.h"
#include "xtc_sync.h"

#include <pthread.h>
#include <string.h>

#define KD_STRIPES 256u

struct kd_slot {
	uint64_t a;                  /* tableid << 32 | frame; 0 = empty */
	uint64_t rid;
};

typedef union {
	struct {
		pthread_mutex_t m;
		uint64_t        seq;     /* odd: a writer is in the stripe */
		uint64_t        ver;     /* change counter (4.5) */
		uint32_t        n;       /* live slots */
		uint32_t        mask;    /* slots - 1 */
		struct kd_slot *slot;
	};
	char pad[128];
} kd_stripe_t;

/* A delta row key.  Keys live as long as their delta: a replace keeps
 * the published key, and the delta frees them all at once, through the
 * `all` chain (cskip has no iterator for a teardown). */
struct kd_key {
	int64_t        rid;          /* first: the cskip key is &rid */
	struct kd_key *all;
};

/* One table's rows in a delta. */
struct kd_tbl {
	uint32_t       tid;
	xtc_cskip_t   *rows;         /* key: &kd_key.rid, value: frame */
	struct kd_tbl *all;
};

struct kd_delta {
	xtc_cskip_t   *tables;       /* key: &kd_tbl.tid */
	uint64_t       rows;
	pthread_mutex_t mk;          /* a table's creation; the key chain */
	struct kd_tbl *tbls;
	struct kd_key *keys;
};

struct kd_runs {
	uint32_t n;
	run_t   *r[];
};

struct keydir {
	kd_stripe_t     *st;
	uint32_t         res_n;
	xtc_arwlock_t   *dlock;      /* writers shared, freeze exclusive */
	struct kd_delta *active, *frozen;
	struct kd_runs  *runs;
};

/* ---- hashing and locks ---- */

static uint64_t
kd_hash(uint32_t tid, int64_t rid)
{
	uint64_t x = (uint64_t)rid ^ ((uint64_t)tid * 0x9E3779B97F4A7C15ull);

	x ^= x >> 30;
	x *= 0xBF58476D1CE4E5B9ull;
	x ^= x >> 27;
	x *= 0x94D049BB133111EBull;
	return x ^ (x >> 31);
}

static void
kd_lock(pthread_mutex_t *m)
{
	xtc_proc_critical_enter();
	(void)pthread_mutex_lock(m);
}

static void
kd_unlock(pthread_mutex_t *m)
{
	(void)pthread_mutex_unlock(m);
	xtc_proc_critical_leave();
}

static kd_stripe_t *
kd_stripe(keydir_t *kd, uint64_t h)
{
	return &kd->st[h & (KD_STRIPES - 1u)];
}

/* ---- resident map ---- */

int
kd_res_get(keydir_t *kd, uint32_t tid, int64_t rid, uint32_t *frame)
{
	uint64_t h = kd_hash(tid, rid), s1, a;
	kd_stripe_t *s = kd_stripe(kd, h);
	uint32_t i, found;

	for (;;) {
		s1 = xtc_atomic_load_u64(&s->seq, xtc_mo_acquire);
		if (s1 & 1u) {
			xtc_yield();         /* a writer: a few instructions */
			continue;
		}
		found = 0;
		for (i = (uint32_t)(h >> 8) & s->mask;;
		    i = (i + 1u) & s->mask) {
			a = xtc_atomic_load_u64(&s->slot[i].a, xtc_mo_acquire);
			if (a == 0)
				break;
			if ((uint32_t)(a >> 32) == tid &&
			    (int64_t)xtc_atomic_load_u64(&s->slot[i].rid,
			    xtc_mo_acquire) == rid) {
				found = (uint32_t)a;
				break;
			}
		}
		if (xtc_atomic_load_u64(&s->seq, xtc_mo_acquire) == s1) {
			if (found != 0 && frame != NULL)
				*frame = found;
			return found != 0;
		}
	}
}

uint64_t
kd_res_ver(keydir_t *kd, uint32_t tid, int64_t rid)
{
	return xtc_atomic_load_u64(&kd_stripe(kd, kd_hash(tid, rid))->ver,
	    xtc_mo_acquire);
}

/* Stripe lock held: the slot of RID, or -1 (and *empty the first free
 * slot of its probe). */
static int
kd_find_locked(kd_stripe_t *s, uint64_t h, uint32_t tid, int64_t rid,
    uint32_t *empty)
{
	uint32_t i;
	uint64_t a;

	for (i = (uint32_t)(h >> 8) & s->mask;; i = (i + 1u) & s->mask) {
		a = xtc_atomic_load_u64(&s->slot[i].a, xtc_mo_relaxed);
		if (a == 0) {
			*empty = i;
			return -1;
		}
		if ((uint32_t)(a >> 32) == tid &&
		    (int64_t)xtc_atomic_load_u64(&s->slot[i].rid,
		    xtc_mo_relaxed) == rid)
			return (int)i;
	}
}

/* Begin / end a change readers must not see half done. */
static void
kd_wbegin(kd_stripe_t *s)
{
	xtc_atomic_fetch_add_u64(&s->seq, 1, xtc_mo_acq_rel);
}

static void
kd_wend(kd_stripe_t *s)
{
	xtc_atomic_fetch_add_u64(&s->seq, 1, xtc_mo_release);
}

/* Stripe lock held: store RID -> frame at slot i (new or existing). */
static void
kd_store(kd_stripe_t *s, uint32_t i, uint32_t tid, int64_t rid,
    uint32_t frame)
{
	xtc_atomic_store_u64(&s->slot[i].rid, (uint64_t)rid, xtc_mo_release);
	xtc_atomic_store_u64(&s->slot[i].a, (uint64_t)tid << 32 | frame,
	    xtc_mo_release);
}

int
kd_res_put(keydir_t *kd, uint32_t tid, int64_t rid, uint32_t frame)
{
	uint64_t h = kd_hash(tid, rid);
	kd_stripe_t *s = kd_stripe(kd, h);
	uint32_t e = 0;
	int i, rc = XTC_OK;

	if (frame == 0)
		return XTC_E_INVAL;
	kd_lock(&s->m);
	i = kd_find_locked(s, h, tid, rid, &e);
	if (i < 0 && (s->n + 1u) * 4u > (s->mask + 1u) * 3u) {
		rc = XTC_E_RESOURCE;         /* 75% full: refuse, never loop */
	} else {
		kd_wbegin(s);
		kd_store(s, i >= 0 ? (uint32_t)i : e, tid, rid, frame);
		if (i < 0) {
			s->n++;
			xtc_atomic_fetch_add_u32(&kd->res_n, 1, xtc_mo_relaxed);
		}
		xtc_atomic_fetch_add_u64(&s->ver, 1, xtc_mo_release);
		kd_wend(s);
	}
	kd_unlock(&s->m);
	return rc;
}

int
kd_res_publish(keydir_t *kd, uint32_t tid, int64_t rid, uint32_t frame,
    uint64_t ver, uint32_t *cur)
{
	uint64_t h = kd_hash(tid, rid);
	kd_stripe_t *s = kd_stripe(kd, h);
	uint32_t e = 0;
	int i, ok = 0;

	if (cur != NULL)
		*cur = 0;
	if (frame == 0)
		return 0;
	kd_lock(&s->m);
	i = kd_find_locked(s, h, tid, rid, &e);
	if (i >= 0) {
		if (cur != NULL)
			*cur = (uint32_t)xtc_atomic_load_u64(&s->slot[i].a,
			    xtc_mo_relaxed);
	} else if (xtc_atomic_load_u64(&s->ver, xtc_mo_relaxed) == ver &&
	    (s->n + 1u) * 4u <= (s->mask + 1u) * 3u) {
		kd_wbegin(s);
		kd_store(s, e, tid, rid, frame);
		s->n++;
		xtc_atomic_fetch_add_u32(&kd->res_n, 1, xtc_mo_relaxed);
		kd_wend(s);
		ok = 1;
	}
	kd_unlock(&s->m);
	return ok;
}

int
kd_res_del(keydir_t *kd, uint32_t tid, int64_t rid, uint32_t frame,
    int bump)
{
	uint64_t h = kd_hash(tid, rid), a;
	kd_stripe_t *s = kd_stripe(kd, h);
	uint32_t e = 0, j, k, home;
	int i;

	kd_lock(&s->m);
	i = kd_find_locked(s, h, tid, rid, &e);
	if (i < 0 || (uint32_t)xtc_atomic_load_u64(&s->slot[i].a,
	    xtc_mo_relaxed) != frame) {
		kd_unlock(&s->m);
		return 0;
	}
	/* Backward-shift deletion: pull each later entry of the probe run
	 * back into the hole when its home allows it. */
	kd_wbegin(s);
	j = (uint32_t)i;
	for (k = (j + 1u) & s->mask;; k = (k + 1u) & s->mask) {
		a = xtc_atomic_load_u64(&s->slot[k].a, xtc_mo_relaxed);
		if (a == 0)
			break;
		home = (uint32_t)(kd_hash((uint32_t)(a >> 32),
		    (int64_t)xtc_atomic_load_u64(&s->slot[k].rid,
		    xtc_mo_relaxed)) >> 8) & s->mask;
		/* k's entry may move to j if j is cyclically in [home, k). */
		if (((k - home) & s->mask) >= ((k - j) & s->mask)) {
			kd_store(s, j, (uint32_t)(a >> 32),
			    (int64_t)xtc_atomic_load_u64(&s->slot[k].rid,
			    xtc_mo_relaxed), (uint32_t)a);
			j = k;
		}
	}
	xtc_atomic_store_u64(&s->slot[j].a, 0, xtc_mo_release);
	s->n--;
	xtc_atomic_fetch_sub_u32(&kd->res_n, 1, xtc_mo_relaxed);
	if (bump)
		xtc_atomic_fetch_add_u64(&s->ver, 1, xtc_mo_release);
	kd_wend(s);
	kd_unlock(&s->m);
	return 1;
}

uint32_t
kd_res_count(keydir_t *kd)
{
	return xtc_atomic_load_u32(&kd->res_n, xtc_mo_relaxed);
}

/* ---- delta ---- */

/* Descending order (see the file comment). */
static int
kd_cmp_rid(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;

	return x < y ? 1 : x > y ? -1 : 0;
}

static int
kd_cmp_tid(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? 1 : x > y ? -1 : 0;
}

static struct kd_delta *
kd_delta_new(void)
{
	struct kd_delta *d = xtc_calloc(1, sizeof *d);

	if (d == NULL)
		return NULL;
	if (xtc_cskip_create(kd_cmp_tid, &d->tables) != XTC_OK) {
		xtc_free(d);
		return NULL;
	}
	(void)pthread_mutex_init(&d->mk, NULL);
	return d;
}

/* Free a delta nobody can reach any more (RCU callback, or destroy). */
static void
kd_delta_free(void *p)
{
	struct kd_delta *d = p;
	struct kd_tbl *t;
	struct kd_key *k;

	if (d == NULL)
		return;
	/* Unreachable now (retired, or the keydir is quiesced): destroy the
	 * maps, then free what they pointed at. */
	while ((t = d->tbls) != NULL) {
		d->tbls = t->all;
		xtc_cskip_destroy(t->rows);
		xtc_free(t);
	}
	while ((k = d->keys) != NULL) {
		d->keys = k->all;
		xtc_free(k);
	}
	xtc_cskip_destroy(d->tables);
	(void)pthread_mutex_destroy(&d->mk);
	xtc_free(d);
}

/* The table map of tid in d, created if `make`.  RCU read-side held. */
static struct kd_tbl *
kd_tbl_of(struct kd_delta *d, uint32_t tid, int make)
{
	struct kd_tbl *t = NULL;
	void *v;

	if (xtc_cskip_get(d->tables, &tid, &v) == XTC_OK)
		return v;
	if (!make)
		return NULL;
	kd_lock(&d->mk);
	if (xtc_cskip_get(d->tables, &tid, &v) == XTC_OK) {
		t = v;
	} else if ((t = xtc_calloc(1, sizeof *t)) != NULL) {
		t->tid = tid;
		if (xtc_cskip_create(kd_cmp_rid, &t->rows) != XTC_OK ||
		    xtc_cskip_insert(d->tables, &t->tid, t, NULL) != XTC_OK) {
			if (t->rows != NULL)
				xtc_cskip_destroy(t->rows);
			xtc_free(t);
			t = NULL;
		} else {
			t->all = d->tbls;
			d->tbls = t;
		}
	}
	kd_unlock(&d->mk);
	return t;
}

int
kd_delta_put(keydir_t *kd, uint32_t tid, int64_t rid, uint32_t frame)
{
	struct kd_delta *d;
	struct kd_tbl *t;
	struct kd_key *key;
	void *old = NULL;
	int rc;

	if (frame == 0)
		return XTC_E_INVAL;
	if ((key = xtc_malloc(sizeof *key)) == NULL)
		return XTC_E_NOMEM;
	key->rid = rid;
	(void)xtc_arwlock_rdlock(kd->dlock, -1);
	xtc_rcu_read_lock();
	d = xtc_atomic_load_ptr(&kd->active, xtc_mo_acquire);
	if ((t = kd_tbl_of(d, tid, 1)) == NULL) {
		rc = XTC_E_NOMEM;
	} else {
		/* A frame is never 0, so a replace returns a non-NULL old
		 * value: that is how a fresh insert is told apart. */
		rc = xtc_cskip_insert(t->rows, &key->rid,
		    (void *)(uintptr_t)frame, &old);
		if (rc == XTC_OK && old == NULL) {
			xtc_atomic_fetch_add_u64(&d->rows, 1, xtc_mo_relaxed);
			kd_lock(&d->mk);
			key->all = d->keys;
			d->keys = key;
			kd_unlock(&d->mk);
			key = NULL;
		}
	}
	xtc_rcu_read_unlock();
	(void)xtc_arwlock_unlock(kd->dlock);
	/* A replace keeps the map's own key: ours was never published. */
	xtc_free(key);
	return rc;
}

/* RID in one delta (NULL: none). */
static int
kd_delta_get1(struct kd_delta *d, uint32_t tid, int64_t rid,
    uint32_t *frame)
{
	struct kd_tbl *t;
	void *v;

	if (d == NULL || (t = kd_tbl_of(d, tid, 0)) == NULL ||
	    xtc_cskip_get(t->rows, &rid, &v) != XTC_OK)
		return 0;
	if (frame != NULL)
		*frame = (uint32_t)(uintptr_t)v;
	return 1;
}

int
kd_delta_get(keydir_t *kd, uint32_t tid, int64_t rid, uint32_t *frame)
{
	return kd_delta_get1(xtc_atomic_load_ptr(&kd->active,
	    xtc_mo_acquire), tid, rid, frame) ||
	    kd_delta_get1(xtc_atomic_load_ptr(&kd->frozen, xtc_mo_acquire),
	    tid, rid, frame);
}

uint64_t
kd_delta_rows(keydir_t *kd)
{
	uint64_t n;

	xtc_rcu_read_lock();
	n = xtc_atomic_load_u64(&((struct kd_delta *)xtc_atomic_load_ptr(
	    &kd->active, xtc_mo_acquire))->rows, xtc_mo_relaxed);
	xtc_rcu_read_unlock();
	return n;
}

int
kd_freeze(keydir_t *kd, void (*at_freeze)(void *ctx), void *ctx)
{
	struct kd_delta *n;
	int rc = XTC_OK;

	if (xtc_atomic_load_ptr(&kd->frozen, xtc_mo_acquire) != NULL)
		return XTC_E_AGAIN;
	if ((n = kd_delta_new()) == NULL)
		return XTC_E_NOMEM;
	/* Exclusive: every commit that already holds the active delta
	 * finishes its insert first, and no new one starts. */
	(void)xtc_arwlock_wrlock(kd->dlock, -1);
	if (xtc_atomic_load_ptr(&kd->frozen, xtc_mo_acquire) != NULL) {
		rc = XTC_E_AGAIN;
	} else {
		xtc_atomic_store_ptr(&kd->frozen,
		    xtc_atomic_load_ptr(&kd->active, xtc_mo_relaxed),
		    xtc_mo_release);
		xtc_atomic_store_ptr(&kd->active, n, xtc_mo_release);
		n = NULL;
		if (at_freeze != NULL)
			at_freeze(ctx);
	}
	(void)xtc_arwlock_unlock(kd->dlock);
	if (n != NULL)
		kd_delta_free(n);
	return rc;
}

uint64_t
kd_flush_boundary(const uint64_t *entry_wa, uint32_t n, uint64_t wal_end)
{
	uint64_t b = wal_end;
	uint32_t i;

	for (i = 0; i < n; i++)
		if (entry_wa[i] != 0 && entry_wa[i] < b)
			b = entry_wa[i];
	return b;
}

int
kd_has_frozen(keydir_t *kd)
{
	return xtc_atomic_load_ptr(&kd->frozen, xtc_mo_acquire) != NULL;
}

int
kd_delta_next(keydir_t *kd, int frozen, uint32_t *tid, int64_t *rid,
    uint32_t *frame)
{
	struct kd_delta *d = xtc_atomic_load_ptr(frozen ? &kd->frozen :
	    &kd->active, xtc_mo_acquire);
	uint32_t want = *tid;
	int64_t from = *rid;
	struct kd_tbl *t;
	void *k, *v;

	if (d == NULL)
		return 0;
	for (;;) {
		/* The first table with id >= want. */
		if (xtc_cskip_floor(d->tables, &want, &k, &v) != XTC_OK)
			return 0;
		t = v;
		if (t->tid != *tid)
			from = INT64_MIN;
		if (xtc_cskip_floor(t->rows, &from, &k, &v) == XTC_OK) {
			*tid = t->tid;
			*rid = *(const int64_t *)k;
			*frame = (uint32_t)(uintptr_t)v;
			return 1;
		}
		if (t->tid == UINT32_MAX)
			return 0;
		want = t->tid + 1u;
		*tid = want;
		from = INT64_MIN;
	}
}

void
kd_drop_frozen(keydir_t *kd)
{
	struct kd_delta *d = xtc_atomic_exchange_ptr(&kd->frozen, NULL,
	    xtc_mo_acq_rel);

	if (d != NULL)
		xtc_rcu_retire(d, kd_delta_free);
}

/* ---- static stage ---- */

int
kd_runs_set(keydir_t *kd, run_t *const *runs, uint32_t n)
{
	struct kd_runs *rs, *old;

	if ((rs = xtc_malloc(sizeof *rs + (size_t)n * sizeof rs->r[0])) ==
	    NULL)
		return XTC_E_NOMEM;
	rs->n = n;
	if (n > 0)
		memcpy(rs->r, runs, (size_t)n * sizeof rs->r[0]);
	old = xtc_atomic_exchange_ptr(&kd->runs, rs, xtc_mo_acq_rel);
	if (old != NULL)
		xtc_rcu_retire(old, xtc_free);
	return XTC_OK;
}

int
kd_lookup(keydir_t *kd, uint32_t tid, int64_t rid, kd_loc_t *out)
{
	struct kd_runs *rs;
	uint32_t i, page;
	uint64_t rank;
	int rc;

	memset(out, 0, sizeof *out);
	if (kd_res_get(kd, tid, rid, &out->frame)) {
		out->kind = KD_FRAME;
		return XTC_OK;
	}
	rs = xtc_atomic_load_ptr(&kd->runs, xtc_mo_acquire);
	for (i = 0; rs != NULL && i < rs->n; i++) {
		if (!run_may_hold(rs->r[i], tid, rid))
			continue;
		rc = run_find(rs->r[i], tid, rid, &rank, &page);
		if (rc == XTC_E_NOTFOUND)
			continue;
		if (rc != XTC_OK)
			return rc;
		out->kind = KD_RUN;
		out->run = rs->r[i];
		out->rank = rank;
		out->page = page;
		return XTC_OK;
	}
	return XTC_OK;
}

/* ---- lifecycle ---- */

int
kd_create(uint32_t cap, keydir_t **out)
{
	keydir_t *kd;
	uint32_t per, n = 16, i;

	if (out == NULL)
		return XTC_E_INVAL;
	if ((kd = xtc_calloc(1, sizeof *kd)) == NULL)
		return XTC_E_NOMEM;
	/* Each stripe at most half full when the map holds cap entries. */
	per = (uint32_t)(((uint64_t)cap * 2u + KD_STRIPES - 1u) / KD_STRIPES);
	while (n < per)
		n <<= 1;
	if ((kd->st = xtc_aligned_alloc(128, KD_STRIPES * sizeof *kd->st)) ==
	    NULL)
		goto fail;
	memset(kd->st, 0, KD_STRIPES * sizeof *kd->st);
	for (i = 0; i < KD_STRIPES; i++) {
		(void)pthread_mutex_init(&kd->st[i].m, NULL);
		kd->st[i].mask = n - 1u;
		if ((kd->st[i].slot = xtc_calloc(n, sizeof(struct kd_slot))) ==
		    NULL)
			goto fail;
	}
	if (xtc_arwlock_create(&kd->dlock) != XTC_OK ||
	    (kd->active = kd_delta_new()) == NULL)
		goto fail;
	*out = kd;
	return XTC_OK;
fail:
	kd_destroy(kd);
	return XTC_E_NOMEM;
}

void
kd_destroy(keydir_t *kd)
{
	uint32_t i;

	if (kd == NULL)
		return;
	if (kd->st != NULL) {
		for (i = 0; i < KD_STRIPES; i++) {
			xtc_free(kd->st[i].slot);
			(void)pthread_mutex_destroy(&kd->st[i].m);
		}
		xtc_aligned_free(kd->st);
	}
	if (kd->dlock != NULL)
		xtc_arwlock_destroy(kd->dlock);
	kd_delta_free(kd->active);
	kd_delta_free(kd->frozen);
	xtc_free(kd->runs);
	xtc_free(kd);
}
