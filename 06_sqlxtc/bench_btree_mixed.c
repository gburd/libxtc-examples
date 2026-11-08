/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/bench_btree_mixed.c
 *	Latency of B-tree reads while other fibers delete and re-insert
 *	(the MVCC garbage-collector / index-maintenance shape).
 *
 *	L loops; R reader fibers and W writer fibers per loop.  Readers do
 *	random bt_lookup and record every latency into a log2 histogram;
 *	writers delete a random key and insert it back (so the tree size is
 *	stable and the delete path runs continuously).  Reports reads/s,
 *	writes/s and read p50 / p99 / p99.9 / max.
 *
 *	Why: RethinkDB's write descent holds the ROOT write-locked across a
 *	child read, which serializes every later reader (operations.cc:
 *	31,432,439) -- it needs page snapshots and 8 shards to stay usable.
 *	sqlxtc's bt_delete did the same (exclusive coupling from the root),
 *	which makes every optimistic (OLC) lookup fail at the root while a
 *	delete is in flight.  This bench is the before/after number for
 *	moving the delete to a shared descent with only the leaf exclusive.
 *
 *	usage: bench_btree_mixed [keys] [seconds] [loops] [readers/loop]
 *	                         [writers/loop]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "bufmgr.h"
#include "btree.h"

#define NB 48          /* log2 ns buckets */

static bm_t *g_bm;
static bt_t *g_bt;
static long g_keys;
static int64_t g_stop_at;
static int g_go, g_left;
static long long g_reads, g_writes, g_miss, g_wfail;
static long long g_hist[NB];
static long long g_max;

static void
mkkey(long i, char *k)
{
	snprintf(k, 24, "k%010ld", i);
}

static int
bucket(int64_t ns)
{
	int b = 0;
	while (ns > 1 && b < NB - 1) { ns >>= 1; b++; }
	return b;
}

static void
reader(void *arg)
{
	uint64_t r = (uint64_t)(uintptr_t)arg * 0x9E3779B97F4A7C15ull + 7;
	char k[24], buf[64];
	uint16_t vl;
	long long n = 0, miss = 0, mx = 0;
	long long h[NB];
	int i;

	memset(h, 0, sizeof h);
	while (!xtc_atomic_load_i32(&g_go, xtc_mo_seq_cst))
		xtc_yield();
	while (xtc_clock_mono() < g_stop_at) {
		int64_t t0, d;
		for (i = 0; i < 32; i++) {
			r = r * 6364136223846793005ull + 1442695040888963407ull;
			mkkey((long)((r >> 33) % (uint64_t)g_keys), k);
			t0 = xtc_clock_mono();
			if (bt_lookup(g_bt, k, 11, buf, sizeof buf, &vl) != XTC_OK)
				miss++;
			d = xtc_clock_mono() - t0;
			h[bucket(d)]++;
			if (d > mx) mx = d;
			n++;
		}
		xtc_yield();
	}
	for (i = 0; i < NB; i++)
		if (h[i])
			xtc_atomic_fetch_add_llong(&g_hist[i], h[i],
			    xtc_mo_seq_cst);
	xtc_atomic_fetch_add_llong(&g_reads, n, xtc_mo_seq_cst);
	xtc_atomic_fetch_add_llong(&g_miss, miss, xtc_mo_seq_cst);
	for (;;) {
		long long cur = xtc_atomic_load_llong(&g_max, xtc_mo_seq_cst);
		if (mx <= cur ||
		    xtc_atomic_cas_weak_llong(&g_max, &cur, mx, xtc_mo_seq_cst,
		        xtc_mo_seq_cst))
			break;
	}
	if (xtc_atomic_fetch_sub_i32(&g_left, 1, xtc_mo_seq_cst) == 1)
		bm_provider_stop(g_bm);
}

static void
writer(void *arg)
{
	uint64_t r = (uint64_t)(uintptr_t)arg * 0xD1B54A32D192ED03ull + 3;
	char k[24], v[32];
	long long n = 0, fail = 0;
	int i;

	while (!xtc_atomic_load_i32(&g_go, xtc_mo_seq_cst))
		xtc_yield();
	while (xtc_clock_mono() < g_stop_at) {
		for (i = 0; i < 8; i++) {
			long id;
			r = r * 6364136223846793005ull + 1442695040888963407ull;
			id = (long)((r >> 33) % (uint64_t)g_keys);
			mkkey(id, k);
			snprintf(v, sizeof v, "v%010ld", id);
			if (bt_delete(g_bt, k, 11) != XTC_OK)
				fail++;
			if (bt_insert(g_bt, k, 11, v, 11) != XTC_OK)
				fail++;
			n++;
		}
		xtc_yield();
	}
	xtc_atomic_fetch_add_llong(&g_writes, n, xtc_mo_seq_cst);
	xtc_atomic_fetch_add_llong(&g_wfail, fail, xtc_mo_seq_cst);
	if (xtc_atomic_fetch_sub_i32(&g_left, 1, xtc_mo_seq_cst) == 1)
		bm_provider_stop(g_bm);
}

static int64_t
pct(double q, long long total)
{
	long long acc = 0, want = (long long)(q * (double)total);
	int i;
	for (i = 0; i < NB; i++) {
		acc += xtc_atomic_load_llong(&g_hist[i], xtc_mo_seq_cst);
		if (acc >= want && acc > 0)
			return (int64_t)1 << i;   /* bucket upper bound (2^i ns) */
	}
	return 0;
}

int
main(int argc, char **argv)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256] = "/tmp/sqlxtc-btmix-XXXXXX";
	long keys = argc > 1 ? atol(argv[1]) : 200000;
	int secs = argc > 2 ? atoi(argv[2]) : 5;
	int L = argc > 3 ? atoi(argv[3]) : 4;
	int R = argc > 4 ? atoi(argv[4]) : 4;
	int W = argc > 5 ? atoi(argv[5]) : 1;
	xtc_exec_t *e = NULL;
	char k[24], v[32];
	long i;
	int fd, j;
	long long reads, writes;
	double dt;

	g_keys = keys;
	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);
	bo.path = path; bo.page_size = 4096; bo.n_frames = 32768; bo.cool_pct = 20;
	if (bm_create(&bo, &g_bm) != XTC_OK || bt_open(g_bm, &g_bt) != XTC_OK)
		return 1;
	for (i = 0; i < keys; i++) {
		mkkey(i, k);
		snprintf(v, sizeof v, "v%010ld", i);
		if (bt_insert(g_bt, k, 11, v, 11) != XTC_OK) return 1;
	}
	if (xtc_exec_init(&e, L) != XTC_OK) return 1;
	if (bm_provider_spawn(g_bm, xtc_exec_loop(e, 0), 1000000LL, NULL) != XTC_OK)
		return 1;
	xtc_atomic_store_i32(&g_left, L * (R + W), xtc_mo_seq_cst);
	for (j = 0; j < L; j++) {
		int q;
		for (q = 0; q < R; q++)
			(void)xtc_proc_spawn(xtc_exec_loop(e, j), reader,
			    (void *)(intptr_t)(j * 100 + q + 1), NULL, NULL);
		for (q = 0; q < W; q++)
			(void)xtc_proc_spawn(xtc_exec_loop(e, j), writer,
			    (void *)(intptr_t)(j * 100 + q + 50), NULL, NULL);
	}
	g_stop_at = xtc_clock_mono() + (int64_t)secs * 1000000000LL;
	xtc_atomic_store_i32(&g_go, 1, xtc_mo_seq_cst);
	(void)xtc_exec_run(e);
	xtc_exec_fini(e);
	reads = xtc_atomic_load_llong(&g_reads, xtc_mo_seq_cst);
	writes = xtc_atomic_load_llong(&g_writes, xtc_mo_seq_cst);
	dt = (double)secs;
	printf(
	    "btmix keys=%ld L=%d R=%d W=%d: reads/s=%.0f writes(del+ins)/s=%.0f "
	    "read p50=%lldns p99=%lldns p99.9=%lldns max=%lldns misses=%lld wfail=%lld\n",
	    keys, L, R, W, (double)reads / dt, (double)writes / dt,
	    (long long)pct(0.50, reads), (long long)pct(0.99, reads),
	    (long long)pct(0.999, reads),
	    (long long)xtc_atomic_load_llong(&g_max, xtc_mo_seq_cst),
	    (long long)xtc_atomic_load_llong(&g_miss, xtc_mo_seq_cst),
	    (long long)xtc_atomic_load_llong(&g_wfail, xtc_mo_seq_cst));
	bt_close(g_bt);
	bm_destroy(g_bm);
	unlink(path);
	return 0;
}
