/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_keydir.c
 *	The keydir (keydir.c, STORAGE_DESIGN 5):
 *	  resident   put / get / replace / delete against a reference map,
 *	             through enough random keys to force long probe runs
 *	             and backward shifts; a full map refuses, never loops;
 *	  publish    a miss publishes only if no commit or drop moved the
 *	             stripe's change counter (4.5): a commit during the miss
 *	             wins, two misses on one RID publish once, an eviction
 *	             does not move the counter and a tombstone drop does;
 *	  delta      puts into the active delta, a freeze, puts into the
 *	             new delta, an ordered walk of each (RID order across
 *	             tables, the newest frame per RID), back-pressure (a
 *	             second freeze is refused) and the drop;
 *	  lookup     a property test: random rounds of commits (resident +
 *	             delta), freezes and flushes into run files, evictions
 *	             and tombstones; kd_lookup always agrees with a
 *	             reference of where each RID's newest version is, and a
 *	             run copy hidden by a newer run is never the answer;
 *	  threads    four threads put, delete and read the resident map at
 *	             once; a reader never sees a frame stored for another
 *	             key.
 *	Prints the resident-map hit cost and the miss cost over N runs
 *	(5.5), and the static stage's bytes per row (5.3).
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btnode.h"
#include "bufmgr.h"
#include "keydir.h"
#include "run.h"
#include "wal.h"
#include "xtc.h"
#include "xtc_rcu.h"
#include "t_tmp.h"

#define PS 4096

static char g_dir[256];
static uint64_t g_rng = 0x4B455944u;

static uint64_t
rnd(void)
{
	uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

/* ---- resident map against a reference ---- */

#define RN 20000

static void
t_resident(void)
{
	static uint32_t ref[RN];       /* frame per key index, 0 = absent */
	keydir_t *kd = NULL;
	uint32_t f, i, k, live = 0, bad = 0;

	CK(kd_create(RN, &kd) == XTC_OK);
	if (kd == NULL)
		return;
	memset(ref, 0, sizeof ref);
	for (i = 0; i < 400000; i++) {
		k = (uint32_t)(rnd() % RN);
		/* Key k: table k % 7, rowid spread so stripes collide. */
		switch (rnd() % 3) {
		case 0:
			f = 1u + (uint32_t)(rnd() % 1000000u);
			CK(kd_res_put(kd, k % 7u, (int64_t)k * 4099, f) ==
			    XTC_OK);
			live += ref[k] == 0;
			ref[k] = f;
			break;
		case 1:
			if (ref[k] != 0) {
				CK(kd_res_del(kd, k % 7u, (int64_t)k * 4099,
				    ref[k] + 1u, 0) == 0);      /* wrong frame */
				CK(kd_res_del(kd, k % 7u, (int64_t)k * 4099,
				    ref[k], (int)(i & 1u)) == 1);
				ref[k] = 0;
				live--;
			}
			break;
		default:
			f = 0;
			if (kd_res_get(kd, k % 7u, (int64_t)k * 4099, &f) !=
			    (ref[k] != 0) || f != ref[k])
				bad++;
		}
	}
	for (k = 0; k < RN; k++) {
		f = 0;
		if (kd_res_get(kd, k % 7u, (int64_t)k * 4099, &f) !=
		    (ref[k] != 0) || f != ref[k])
			bad++;
	}
	CK(bad == 0 && kd_res_count(kd) == live);
	kd_destroy(kd);
	/* A full map refuses instead of probing forever. */
	CK(kd_create(16, &kd) == XTC_OK);
	for (i = 0, k = 0; kd != NULL && i < 100000; i++)
		if (kd_res_put(kd, 1, (int64_t)i, 1) == XTC_E_RESOURCE)
			k++;
	CK(k > 0);
	kd_destroy(kd);
	printf("  resident: 400k ops against a reference map, %u live; a "
	    "full map refuses\n", live);
}

static void
t_publish(void)
{
	keydir_t *kd = NULL;
	uint64_t v;
	uint32_t cur;

	CK(kd_create(1000, &kd) == XTC_OK);
	if (kd == NULL)
		return;
	/* Two misses on RID (3, 9): the first publishes, the second sees
	 * its frame. */
	v = kd_res_ver(kd, 3, 9);
	CK(kd_res_publish(kd, 3, 9, 11, v, &cur) == 1);
	CK(kd_res_publish(kd, 3, 9, 12, v, &cur) == 0 && cur == 11);
	/* An eviction does not move the counter: a miss sampled before it
	 * may still publish. */
	v = kd_res_ver(kd, 3, 9);
	CK(kd_res_del(kd, 3, 9, 11, 0) == 1);
	CK(kd_res_ver(kd, 3, 9) == v);
	CK(kd_res_publish(kd, 3, 9, 13, v, &cur) == 1);
	/* A commit during a miss wins: the miss's sample is stale. */
	CK(kd_res_del(kd, 3, 9, 13, 0) == 1);
	v = kd_res_ver(kd, 3, 9);
	CK(kd_res_put(kd, 3, 9, 20) == XTC_OK);
	CK(kd_res_del(kd, 3, 9, 20, 0) == 1);       /* even if evicted since */
	CK(kd_res_publish(kd, 3, 9, 14, v, &cur) == 0 && cur == 0);
	/* A tombstone drop moves it too. */
	v = kd_res_ver(kd, 3, 9);
	CK(kd_res_publish(kd, 3, 9, 15, v, &cur) == 1);
	CK(kd_res_del(kd, 3, 9, 15, 1) == 1);
	CK(kd_res_publish(kd, 3, 9, 16, v, &cur) == 0 && cur == 0);
	kd_destroy(kd);
	printf("  publish: a commit or a drop during a miss wins; an eviction "
	    "does not stop a publish\n");
}

/* The at-freeze hook runs once, at the switch. */
static void
at_freeze(void *ctx)
{
	(*(uint32_t *)ctx)++;
}

static void
t_delta(void)
{
	keydir_t *kd = NULL;
	uint32_t tid, f, n;
	int64_t rid, last;
	uint32_t lt;

	CK(kd_create(1000, &kd) == XTC_OK);
	if (kd == NULL)
		return;
	n = 0;
	xtc_rcu_read_lock();
	CK(kd_delta_put(kd, 5, 100, 1) == XTC_OK);
	CK(kd_delta_put(kd, 2, -7, 2) == XTC_OK);
	CK(kd_delta_put(kd, 5, 50, 3) == XTC_OK);
	CK(kd_delta_put(kd, 5, 100, 4) == XTC_OK);   /* replace */
	CK(kd_delta_put(kd, UINT32_MAX, INT64_MAX, 5) == XTC_OK);
	CK(kd_delta_put(kd, 0, INT64_MIN, 6) == XTC_OK);
	CK(kd_delta_rows(kd) == 5);
	CK(kd_freeze(kd, at_freeze, &n) == XTC_OK && n == 1);
	CK(kd_freeze(kd, NULL, NULL) == XTC_E_AGAIN && kd_has_frozen(kd));
	CK(kd_delta_put(kd, 5, 100, 7) == XTC_OK);
	CK(kd_delta_rows(kd) == 1);
	CK(kd_delta_get(kd, 5, 100, &f) && f == 7);  /* active first */
	CK(kd_delta_get(kd, 5, 50, &f) && f == 3);   /* then frozen */
	CK(!kd_delta_get(kd, 5, 51, &f));
	/* The frozen delta in RID order. */
	tid = 0;
	rid = INT64_MIN;
	n = 0;
	lt = 0;
	last = INT64_MIN;
	while (kd_delta_next(kd, 1, &tid, &rid, &f)) {
		CK(n == 0 || tid > lt || (tid == lt && rid > last));
		static const uint32_t wf[] = { 6, 2, 3, 4, 5 };
		CK(n < 5 && f == wf[n]);
		n++;
		lt = tid;
		last = rid;
		if (rid == INT64_MAX) {
			if (tid == UINT32_MAX)
				break;
			tid++;
			rid = INT64_MIN;
		} else {
			rid++;
		}
	}
	CK(n == 5);
	xtc_rcu_read_unlock();
	kd_drop_frozen(kd);
	CK(!kd_has_frozen(kd) && kd_freeze(kd, NULL, NULL) == XTC_OK);
	kd_drop_frozen(kd);
	xtc_rcu_synchronize();
	kd_destroy(kd);
	printf("  delta: freeze, back-pressure, ordered walk across tables, "
	    "newest frame per RID\n");
}

/* ---- lookup property test ---- */

#define LT  3              /* tables */
#define LR  400            /* rowids per table */
#define LRUNS 40

struct where {
	int      kind;         /* 0 none, 1 frame (resident), 2 run */
	uint32_t frame;
	int      run;          /* index into g_runs (creation order) */
	int      dirty;        /* committed since the last flush */
};

static run_t *g_runs[LRUNS];
static int g_nruns;

/* Write the frozen delta of kd into run file `runid` and open it. */
static int
flush_frozen(keydir_t *kd, bm_t *bm, uint64_t runid)
{
	run_writer_t *w = NULL;
	run_meta_t m;
	run_row_t r;
	uint8_t rec[16];
	char path[300];
	uint32_t tid = 0, f;
	int64_t rid = INT64_MIN;
	int rc;

	run_path(g_dir, runid, path, sizeof path);
	if ((rc = run_writer_open(bm, PS, path, runid, 16, &w)) != XTC_OK)
		return rc;
	xtc_rcu_read_lock();
	while (kd_delta_next(kd, 1, &tid, &rid, &f)) {
		memset(&r, 0, sizeof r);
		r.tableid = tid;
		r.rowid = rid;
		r.commit_ts = f;
		memcpy(rec, &f, 4);
		r.len = 4;
		r.rec = rec;
		if ((rc = run_writer_add(w, &r)) != XTC_OK)
			break;
		rid++;
	}
	xtc_rcu_read_unlock();
	if (rc != XTC_OK) {
		run_writer_abort(w);
		return rc;
	}
	if ((rc = run_writer_finish(w, &m)) != XTC_OK)
		return rc;
	return run_open(bm, path, &m, &g_runs[g_nruns++]);
}

static int
runs_newest_first(keydir_t *kd)
{
	run_t *v[LRUNS];
	int i;

	for (i = 0; i < g_nruns; i++)
		v[i] = g_runs[g_nruns - 1 - i];
	return kd_runs_set(kd, v, (uint32_t)g_nruns);
}

static void
t_lookup(bm_t *bm)
{
	static struct where ref[LT][LR];
	static uint32_t in_run[LT][LR];     /* frame value of the newest run */
	keydir_t *kd = NULL;
	uint32_t next_frame = 1, t, f;
	int round, i, bad = 0, nlook = 0, r;
	kd_loc_t loc;
	run_row_t row;
	uint8_t buf[16];

	CK(kd_create(LT * LR * 2, &kd) == XTC_OK);
	if (kd == NULL)
		return;
	memset(ref, 0, sizeof ref);
	memset(in_run, 0, sizeof in_run);
	for (round = 0; round < LRUNS && !g_fail; round++) {
		/* Commits: a resident frame and a delta entry each. */
		for (i = 0; i < 300; i++) {
			t = (uint32_t)(rnd() % LT);
			r = (int)(rnd() % LR);
			f = next_frame++;
			CK(kd_res_put(kd, t + 1, r * 3 - 500, f) == XTC_OK);
			CK(kd_delta_put(kd, t + 1, r * 3 - 500, f) == XTC_OK);
			ref[t][r].kind = 1;
			ref[t][r].frame = f;
			ref[t][r].dirty = 1;
		}
		/* Flush: freeze, write the run, install it, drop the delta.
		 * The frames stay resident until "evicted" below. */
		CK(kd_freeze(kd, NULL, NULL) == XTC_OK);
		CK(flush_frozen(kd, bm, (uint64_t)round + 1u) == XTC_OK);
		CK(runs_newest_first(kd) == XTC_OK);
		kd_drop_frozen(kd);
		for (t = 0; t < LT; t++)
			for (r = 0; r < LR; r++)
				if (ref[t][r].dirty) {
					in_run[t][r] = ref[t][r].frame;
					ref[t][r].run = g_nruns - 1;
					ref[t][r].dirty = 0;
				}
		/* Evict some resident frames: their lookups fall to the
		 * newest run that holds the RID. */
		for (i = 0; i < 400; i++) {
			t = (uint32_t)(rnd() % LT);
			r = (int)(rnd() % LR);
			if (ref[t][r].kind == 1 &&
			    kd_res_del(kd, t + 1, r * 3 - 500, ref[t][r].frame,
			    0) == 1)
				ref[t][r].kind = 2;
		}
		/* Every RID agrees with the reference. */
		xtc_rcu_read_lock();
		for (t = 0; t < LT; t++)
			for (r = 0; r < LR; r++) {
				nlook++;
				if (kd_lookup(kd, t + 1, r * 3 - 500, &loc) !=
				    XTC_OK) {
					bad++;
					continue;
				}
				if (ref[t][r].kind == 0) {
					bad += loc.kind != KD_NONE;
				} else if (ref[t][r].kind == 1) {
					bad += loc.kind != KD_FRAME ||
					    loc.frame != ref[t][r].frame;
				} else if (loc.kind != KD_RUN ||
				    run_read(loc.run, loc.page, t + 1,
				    r * 3 - 500, &row, buf, sizeof buf) !=
				    XTC_OK || row.len != 4 ||
				    memcmp(buf, &in_run[t][r], 4) != 0 ||
				    loc.run != g_runs[ref[t][r].run]) {
					bad++;
				}
			}
		/* RIDs never written are nowhere (between and past them). */
		for (t = 1; t <= LT; t++)
			if (kd_lookup(kd, t, -499, &loc) != XTC_OK ||
			    loc.kind != KD_NONE ||
			    kd_lookup(kd, t + LT, 1, &loc) != XTC_OK ||
			    loc.kind != KD_NONE)
				bad++;
		xtc_rcu_read_unlock();
	}
	CK(bad == 0);
	xtc_rcu_synchronize();
	kd_destroy(kd);
	printf("  lookup: %d rounds of commit/flush/evict, %d lookups agree "
	    "with the reference (newest run wins)\n", round, nlook);
}

/* ---- threads ---- */

#define TK 4096
static keydir_t *g_kd;
static int g_tbad, g_tstop;

/* The frame stored for key k is always a function of k. */
static uint32_t
fr_of(uint32_t k, uint32_t gen)
{
	return ((k * 2654435761u) ^ (gen << 20)) | 1u;
}

static void *
writer(void *arg)
{
	uint64_t s = (uint64_t)(uintptr_t)arg * 977u + 1u;
	uint32_t i, k, gen;

	for (i = 0; i < 300000; i++) {
		s = s * 6364136223846793005ull + 1442695040888963407ull;
		k = (uint32_t)(s >> 33) % TK;
		gen = (uint32_t)(s >> 20) & 7u;
		if ((s >> 40) & 1u)
			(void)kd_res_put(g_kd, 9, (int64_t)k, fr_of(k, gen));
		else
			(void)kd_res_del(g_kd, 9, (int64_t)k, fr_of(k, gen),
			    (int)((s >> 41) & 1u));
	}
	return NULL;
}

static void *
reader(void *arg)
{
	uint32_t k = (uint32_t)(uintptr_t)arg, f, g;
	int ok;

	while (!xtc_atomic_load_i32(&g_tstop, xtc_mo_acquire)) {
		k = (k * 1103515245u + 12345u) % TK;
		f = 0;
		if (!kd_res_get(g_kd, 9, (int64_t)k, &f))
			continue;
		for (g = 0, ok = 0; g < 8; g++)
			ok |= f == fr_of(k, g);
		if (!ok)
			xtc_atomic_store_i32(&g_tbad, 1, xtc_mo_relaxed);
	}
	return NULL;
}

static void
t_threads(void)
{
	pthread_t w[2], r[2];
	int i;

	CK(kd_create(TK, &g_kd) == XTC_OK);
	if (g_kd == NULL)
		return;
	for (i = 0; i < 2; i++) {
		CK(pthread_create(&w[i], NULL, writer, (void *)(uintptr_t)i) ==
		    0);
		CK(pthread_create(&r[i], NULL, reader,
		    (void *)(uintptr_t)(i + 7)) == 0);
	}
	for (i = 0; i < 2; i++)
		(void)pthread_join(w[i], NULL);
	xtc_atomic_store_i32(&g_tstop, 1, xtc_mo_release);
	for (i = 0; i < 2; i++)
		(void)pthread_join(r[i], NULL);
	CK(!g_tbad);
	kd_destroy(g_kd);
	printf("  threads: 2 writers x 300k puts/deletes against 2 lock-free "
	    "readers, no torn read\n");
}

/* ---- costs ---- */

static void
t_costs(bm_t *bm)
{
	keydir_t *kd = NULL;
	kd_loc_t loc;
	char path[300];
	int64_t t0, t1;
	uint32_t i, f, sink = 0;
	int k;

	CK(kd_create(1u << 16, &kd) == XTC_OK);
	if (kd == NULL)
		return;
	for (i = 0; i < 50000; i++)
		CK(kd_res_put(kd, 1, (int64_t)i, i + 1u) == XTC_OK);
	t0 = xtc_clock_mono();
	for (i = 0; i < 2000000; i++)
		if (kd_res_get(kd, 1, (int64_t)((i * 2654435761u) % 50000u),
		    &f))
			sink += f;
	t1 = xtc_clock_mono();
	printf("  cost: resident-map hit %.1f ns\n", (double)(t1 - t0) /
	    2000000.0);
	/* Misses over the runs of t_lookup, newest first: rowids that live
	 * only in the oldest run cost a probe of every run that may hold
	 * them (insert-ordered tables would be excluded by range). */
	CK(runs_newest_first(kd) == XTC_OK);
	xtc_rcu_read_lock();
	t0 = xtc_clock_mono();
	for (k = 0; k < 20000; k++) {
		int r = (int)((uint32_t)k * 2654435761u % LR);
		if (kd_lookup(kd, 1 + (uint32_t)k % LT, r * 3 - 500, &loc) ==
		    XTC_OK && loc.kind == KD_RUN)
			sink += loc.page;
	}
	t1 = xtc_clock_mono();
	xtc_rcu_read_unlock();
	printf("  cost: miss over %d runs (footers resident) %.0f ns\n",
	    g_nruns, (double)(t1 - t0) / 20000.0);
	/* Static-stage bytes per row in a full-size run: one table of
	 * 100k dense rowids, 100-byte rows (5.3's case).  The footer
	 * length is the header's field at byte 80. */
	{
		run_writer_t *w = NULL;
		run_meta_t m;
		run_row_t r;
		uint8_t rec[100], hdr[PS];
		uint64_t fb = 0;
		FILE *fp;

		memset(rec, 'r', sizeof rec);
		run_path(g_dir, 9999, path, sizeof path);
		CK(run_writer_open(bm, PS, path, 9999, 16, &w) == XTC_OK);
		for (i = 0; w != NULL && i < 100000; i++) {
			memset(&r, 0, sizeof r);
			r.tableid = 4;
			r.rowid = (int64_t)i + 1;
			r.len = sizeof rec;
			r.rec = rec;
			CK(run_writer_add(w, &r) == XTC_OK);
		}
		CK(run_writer_finish(w, &m) == XTC_OK);
		if ((fp = fopen(path, "rb")) != NULL &&
		    fread(hdr, 1, PS, fp) == PS) {
			memcpy(&fb, hdr + 80, 8);
			printf("  static stage, 100k dense rowids, 100-byte "
			    "rows: footer %.2f bits/row, resident %.2f "
			    "bits/row\n", 8.0 * (double)fb / (double)m.nrows,
			    8.0 * (40.0 + 16.0 * (double)((m.nrows + 511) /
			    512)) / (double)m.nrows);
			/* 5.3: about 2 + 0.5 + directory; well under 4. */
			CK(8.0 * (double)fb / (double)m.nrows < 4.0);
		}
		if (fp != NULL)
			fclose(fp);
	}
	unlink(path);
	if (sink == 0xFFFFFFFFu)
		printf("%u\n", sink);
	kd_destroy(kd);
}

int
main(void)
{
	bm_opts_t o = BM_OPTS_DEFAULT;
	char p[300];
	bm_t *bm = NULL;
	int i;

	t_tmpl(g_dir, sizeof g_dir, "keydir");
	if (mkdtemp(g_dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	t_resident();
	t_publish();
	t_delta();
	snprintf(p, sizeof p, "%s/pages", g_dir);
	o.path = p;
	o.page_size = PS;
	o.n_frames = 256;
	o.cksum_off = BTNODE_CKSUM_OFF;
	o.cksum = wal_crc32c;
	CK(bm_create(&o, &bm) == XTC_OK);
	if (bm != NULL) {
		t_lookup(bm);
		t_costs(bm);
		for (i = 0; i < g_nruns; i++)
			run_close(g_runs[i]);
		bm_destroy(bm);
	}
	t_threads();
	unlink(p);
	for (i = 1; i <= LRUNS; i++) {
		run_path(g_dir, (uint64_t)i, p, sizeof p);
		unlink(p);
	}
	(void)rmdir(g_dir);
	xtc_rcu_fini();                /* run what the keydirs retired */
	if (!g_fail)
		printf("ok test_keydir\n");
	return g_fail;
}
