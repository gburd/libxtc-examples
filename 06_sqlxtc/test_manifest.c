/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_manifest.c
 *	The MANIFEST (STORAGE_DESIGN 7.2) and the nursery flush (9.2):
 *	  flush      commits into the delta, a flush: a run at level n0 is
 *	             named by a durable manifest with the boundary read
 *	             before the freeze, and the keydir finds every flushed
 *	             row in it once its frame is gone;
 *	  reopen     the manifest restores the runs, the boundary and the
 *	             next run id; every row is found; a run file the
 *	             manifest does not name (a flush that died before its
 *	             install) and a MANIFEST.tmp are deleted;
 *	  refuse     a manifest with a bad CRC, or naming a run whose header
 *	             does not match, refuses the open and deletes nothing;
 *	  missing    a MANIFEST that is gone while run files remain, or that
 *	             cannot be read, refuses the open and deletes nothing:
 *	             never "a new store";
 *	  retry      a flush whose row source fails installs nothing and
 *	             leaves the delta frozen; mf_flush_frozen finishes it;
 *	             an empty delta installs nothing;
 *	             a boundary below the installed one is refused;
 *	  install    a flush whose manifest install fails (MANIFEST.tmp is
 *	             a directory) removes its run, keeps the delta frozen
 *	             and the boundary, and the old MANIFEST still opens;
 *	  renamed    a failure after the manifest's rename (the directory
 *	             fsync) keeps the run it names, fail-stops later
 *	             flushes, and the reopen finds every row;
 *	  text       64 runs with the widest ids, row counts and levels
 *	             encode in full and parse back the same (no run line
 *	             is silently dropped, no write past the buffer);
 *	  reclaim    many flushes with no synchronize of the test's own:
 *	             the live allocations do not grow (each flush reclaims
 *	             its frozen delta and the old run set);
 *	  concurrent committer fibers on two loops keep adding rows while a
 *	             flusher fiber flushes many times (each flush parks on
 *	             its file work); afterwards every row is either in the
 *	             delta or in exactly the run the reference expects.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "btnode.h"
#include "bufmgr.h"
#include "keydir.h"

/* manifest.c is compiled in, so a test can fail the directory fsync
 * that follows a manifest's rename (t_renamed). */
static int g_fail_dirsync;
static int t_renamed_sync(const char *dir);
#define MF_RENAMED_SYNC(dir) t_renamed_sync(dir)
#include "manifest.c"
#include "run.h"
#include "wal.h"
#include "xtc.h"
#include "xtc_alloc_audit.h"
#include "xtc_exec.h"
#include "xtc_fs.h"
#include "xtc_proc.h"
#include "xtc_rcu.h"
#include "xtc_runtime.h"
#include "t_tmp.h"

#define PS 4096

static char g_dir[256];
static int  g_fail_row;              /* the row source fails once > 0 */

/* The row source: frame f is the record "f=<f>" of RID (tid, rid). */
static int
row_of(void *ctx, uint32_t frame, run_row_t *row)
{
	static _Thread_local char rec[32];

	(void)ctx;
	if (g_fail_row > 0 && --g_fail_row == 0)
		return XTC_E_IO;
	row->commit_ts = frame;
	row->self_wa = WAL_WA(1, frame);
	row->len = (uint16_t)snprintf(rec, sizeof rec, "f=%u", frame);
	row->rec = (const uint8_t *)rec;
	return XTC_OK;
}

static uint64_t g_bnd;
static uint64_t
boundary(void *ctx)
{
	(void)ctx;
	return ++g_bnd * 1000u;
}

/* A boundary source that checks it runs before the switch (SF3): the
 * rows are still in the active delta and nothing is frozen. */
static uint64_t
pre_boundary(void *ctx)
{
	keydir_t *kd = ctx;

	CK(!kd_has_frozen(kd) && kd_delta_rows(kd) > 0);
	return boundary(NULL);
}

/* A broken boundary source: below every installed boundary. */
static uint64_t
low_boundary(void *ctx)
{
	(void)ctx;
	return 1;
}

static bm_t *
open_bm_at(const char *name)
{
	bm_opts_t o = BM_OPTS_DEFAULT;
	char p[300];
	bm_t *bm = NULL;

	snprintf(p, sizeof p, "%s/%s", g_dir, name);
	o.path = p;
	o.page_size = PS;
	o.n_frames = 128;
	o.cksum_off = BTNODE_CKSUM_OFF;
	o.cksum = wal_crc32c;
	CK(bm_create(&o, &bm) == XTC_OK);
	return bm;
}

static bm_t *
open_bm(void)
{
	return open_bm_at("pages");
}

/* 1 if the newest place of RID holds frame f (a run copy reads "f=<f>"). */
static int
found_as(keydir_t *kd, uint32_t tid, int64_t rid, uint32_t f)
{
	kd_loc_t loc;
	run_row_t row;
	uint8_t buf[32];
	char want[32];
	int ok = 0, n;

	n = snprintf(want, sizeof want, "f=%u", f);
	xtc_rcu_read_lock();
	if (kd_lookup(kd, tid, rid, &loc) == XTC_OK) {
		if (loc.kind == KD_FRAME)
			ok = loc.frame == f;
		else if (loc.kind == KD_RUN)
			ok = run_read(loc.run, loc.page, tid, rid, &row, buf,
			    sizeof buf) == XTC_OK && row.len == n &&
			    memcmp(buf, want, (size_t)n) == 0;
	}
	xtc_rcu_read_unlock();
	return ok;
}

static int
exists(const char *name)
{
	char p[300];
	struct stat sb;

	snprintf(p, sizeof p, "%s/%s", g_dir, name);
	return stat(p, &sb) == 0;
}

static void
t_flush_reopen(void)
{
	manifest_t *m = NULL;
	keydir_t *kd = NULL;
	bm_t *bm;
	FILE *fp;
	char p[300];
	uint32_t i, bad = 0;

	bm = open_bm();
	CK(kd_create(4096, &kd) == XTC_OK);
	CK(bm != NULL && kd != NULL && mf_open(g_dir, bm, kd, &m) == XTC_OK);
	if (m == NULL)
		return;
	/* A new store installs its empty manifest at once (review S4). */
	CK(mf_nruns(m) == 0 && mf_boundary(m) == 0 && exists("MANIFEST"));
	CK(mf_flush(m, row_of, NULL, boundary, NULL) == XTC_E_NOTFOUND);
	/* Two flushes of 1000 rows over three tables; the second updates
	 * half of the first's rows. */
	for (i = 0; i < 1000; i++)
		CK(kd_delta_put(kd, 1 + i % 3, (int64_t)i, 10000 + i) ==
		    XTC_OK);
	CK(mf_flush(m, row_of, NULL, pre_boundary, kd) == XTC_OK);
	CK(mf_nruns(m) == 1 && mf_boundary(m) == g_bnd * 1000u);
	for (i = 0; i < 1000; i += 2)
		CK(kd_delta_put(kd, 1 + i % 3, (int64_t)i, 20000 + i) ==
		    XTC_OK);
	CK(mf_flush(m, row_of, NULL, boundary, NULL) == XTC_OK);
	CK(mf_nruns(m) == 2 && exists("MANIFEST"));
	for (i = 0; i < 1000; i++)
		bad += !found_as(kd, 1 + i % 3, (int64_t)i,
		    (i & 1u) ? 10000 + i : 20000 + i);
	CK(bad == 0);
	mf_close(m);
	kd_destroy(kd);
	/* A run that never made it into the manifest, and a stale tmp. */
	snprintf(p, sizeof p, "%s/rl-77.run", g_dir);
	if ((fp = fopen(p, "w")) != NULL)
		fclose(fp);
	snprintf(p, sizeof p, "%s/MANIFEST.tmp", g_dir);
	if ((fp = fopen(p, "w")) != NULL)
		fclose(fp);
	CK(kd_create(4096, &kd) == XTC_OK);
	CK(mf_open(g_dir, bm, kd, &m) == XTC_OK);
	if (m == NULL)
		return;
	CK(mf_nruns(m) == 2 && mf_boundary(m) == g_bnd * 1000u);
	CK(!exists("rl-77.run") && !exists("MANIFEST.tmp"));
	for (i = 0, bad = 0; i < 1000; i++)
		bad += !found_as(kd, 1 + i % 3, (int64_t)i,
		    (i & 1u) ? 10000 + i : 20000 + i);
	CK(bad == 0);
	/* The next run id continues past both runs. */
	CK(kd_delta_put(kd, 9, 1, 5) == XTC_OK);
	CK(mf_flush(m, row_of, NULL, boundary, NULL) == XTC_OK);
	CK(exists("rl-3.run") && mf_nruns(m) == 3);
	/* Ids 65535 past live runs 1..3 share their buffer-manager file
	 * numbers: the flush skips 65536..65538 and writes 65539. */
	m->next = 1u + 65535u;
	CK(kd_delta_put(kd, 9, 2, 6) == XTC_OK);
	CK(mf_flush(m, row_of, NULL, boundary, NULL) == XTC_OK);
	CK(mf_nruns(m) == 4 && !exists("rl-65536.run") &&
	    !exists("rl-65538.run") && exists("rl-65539.run"));
	CK(found_as(kd, 9, 2, 6));
	mf_close(m);
	kd_destroy(kd);
	bm_destroy(bm);
	printf("  flush/reopen: runs, boundary and next id survive; newest run "
	    "wins; strays and MANIFEST.tmp deleted\n");
}

static void
t_refuse(void)
{
	manifest_t *m = NULL;
	keydir_t *kd = NULL;
	char p[300], q[300], *t, *at;
	size_t len;
	bm_t *bm;
	FILE *fp;

	bm = open_bm();
	CK(kd_create(4096, &kd) == XTC_OK);
	snprintf(p, sizeof p, "%s/MANIFEST", g_dir);
	CK((fp = fopen(p, "rb")) != NULL);
	if (fp == NULL)
		return;
	t = malloc(1u << 16);
	len = fread(t, 1, 1u << 16, fp);
	fclose(fp);
	/* The boundary's last hex digit flipped (0 <-> 1): the text still
	 * parses, and names the same runs, so only the CRC refuses it. */
	at = strstr(t, "\nboundary ");
	CK(at != NULL);
	if (at == NULL)
		return;
	at += strlen("\nboundary ") + 15;
	*at ^= 1;
	CK((fp = fopen(p, "wb")) != NULL && fwrite(t, 1, len, fp) == len);
	fclose(fp);
	CK(mf_open(g_dir, bm, kd, &m) == XTC_E_IO && m == NULL);
	CK(exists("rl-1.run") && exists("rl-2.run"));
	*at ^= 1;
	CK((fp = fopen(p, "wb")) != NULL && fwrite(t, 1, len, fp) == len);
	fclose(fp);
	/* A run file replaced by another (its header CRC differs). */
	snprintf(p, sizeof p, "%s/rl-2.run", g_dir);
	snprintf(q, sizeof q, "%s/rl-2.keep", g_dir);
	CK(rename(p, q) == 0);
	snprintf(q, sizeof q, "%s/rl-3.run", g_dir);
	CK(link(q, p) == 0);
	CK(mf_open(g_dir, bm, kd, &m) == XTC_E_IO);
	CK(unlink(p) == 0);
	snprintf(q, sizeof q, "%s/rl-2.keep", g_dir);
	CK(rename(q, p) == 0);
	CK(mf_open(g_dir, bm, kd, &m) == XTC_OK && mf_nruns(m) == 4);
	mf_close(m);
	free(t);
	kd_destroy(kd);
	bm_destroy(bm);
	printf("  refuse: a bad CRC or a mismatched run refuses the open, "
	    "nothing deleted\n");
}

/* Rename MANIFEST away (or back): 0 on success. */
static int
mf_move(const char *from, const char *to)
{
	char a[300], b[300];

	snprintf(a, sizeof a, "%s/%s", g_dir, from);
	snprintf(b, sizeof b, "%s/%s", g_dir, to);
	return rename(a, b);
}

static void
t_missing(void)
{
	manifest_t *m = NULL;
	keydir_t *kd = NULL;
	char p[300];
	bm_t *bm;

	bm = open_bm();
	CK(kd_create(4096, &kd) == XTC_OK);
	CK(mf_move("MANIFEST", "MANIFEST.away") == 0);
	CK(mf_open(g_dir, bm, kd, &m) == XTC_E_IO && m == NULL);
	CK(exists("rl-1.run") && exists("rl-2.run") && exists("rl-3.run"));
	CK(!exists("MANIFEST"));
	CK(mf_move("MANIFEST.away", "MANIFEST") == 0);
	/* Unreadable (root reads anything: then only the absent case). */
	snprintf(p, sizeof p, "%s/MANIFEST", g_dir);
	if (geteuid() != 0) {
		CK(chmod(p, 0) == 0);
		CK(mf_open(g_dir, bm, kd, &m) == XTC_E_IO && m == NULL);
		CK(exists("rl-1.run") && exists("rl-3.run"));
		CK(chmod(p, 0600) == 0);
	}
	CK(mf_open(g_dir, bm, kd, &m) == XTC_OK && mf_nruns(m) == 4);
	mf_close(m);
	kd_destroy(kd);
	bm_destroy(bm);
	printf("  missing: a lost or unreadable MANIFEST beside run files "
	    "refuses the open, nothing deleted\n");
}

/*
 * The install fails before its rename: MANIFEST.tmp is a directory, so
 * it cannot be created.  The run file must be gone, the delta still
 * frozen, the boundary and run count unchanged, and the old MANIFEST
 * must still open with the same runs.
 */
static void
t_install_fail(manifest_t *m, keydir_t *kd, uint32_t n0, uint64_t b0)
{
	manifest_t *m2 = NULL;
	keydir_t *kd2 = NULL;
	bm_t *bm2;
	char p[300], name[64];
	uint64_t id = m->next;

	snprintf(p, sizeof p, "%s/MANIFEST.tmp", g_dir);
	CK(mkdir(p, 0700) == 0);
	CK(mf_flush_frozen(m, row_of, NULL) != XTC_OK);
	snprintf(name, sizeof name, "rl-%" PRIu64 ".run", id);
	CK(!exists(name));
	CK(kd_has_frozen(kd) && mf_nruns(m) == n0 && mf_boundary(m) == b0);
	CK(rmdir(p) == 0);
	/* Through a second buffer manager: m's runs stay attached to its
	 * own.  (It must not sweep: m2 sees the same durable manifest.) */
	bm2 = open_bm_at("pages-if");
	CK(kd_create(4096, &kd2) == XTC_OK);
	CK(bm2 != NULL && mf_open(g_dir, bm2, kd2, &m2) == XTC_OK &&
	    m2 != NULL && mf_nruns(m2) == n0 && mf_boundary(m2) == b0);
	mf_close(m2);
	kd_destroy(kd2);
	bm_destroy(bm2);
}

/* A freeze boundary below the installed one is refused before anything
 * is written (it only rises; review N4).  Runs while t_retry holds a
 * frozen delta from boundary(); a second delta is not possible, so
 * swap the frozen boundary to the low one and back. */
static void
t_low_boundary(manifest_t *m, keydir_t *kd, uint32_t n0, uint64_t b0)
{
	uint64_t keep = m->frz_boundary;

	m->frz_boundary = low_boundary(NULL);
	CK(b0 > 1 && mf_flush_frozen(m, row_of, NULL) == XTC_E_INVAL);
	CK(kd_has_frozen(kd) && mf_nruns(m) == n0 && mf_boundary(m) == b0);
	m->frz_boundary = keep;
}

static void
t_retry(void)
{
	manifest_t *m = NULL;
	keydir_t *kd = NULL;
	bm_t *bm;
	uint32_t i, n0, bad = 0;
	uint64_t b0;

	bm = open_bm();
	CK(kd_create(4096, &kd) == XTC_OK);
	CK(mf_open(g_dir, bm, kd, &m) == XTC_OK);
	if (m == NULL)
		return;
	n0 = mf_nruns(m);
	b0 = mf_boundary(m);
	for (i = 0; i < 200; i++)
		CK(kd_delta_put(kd, 4, (int64_t)i, 30000 + i) == XTC_OK);
	g_fail_row = 50;
	CK(mf_flush(m, row_of, NULL, boundary, NULL) == XTC_E_IO);
	CK(mf_nruns(m) == n0 && mf_boundary(m) == b0 && kd_has_frozen(kd));
	CK(mf_flush(m, row_of, NULL, boundary, NULL) == XTC_E_AGAIN);
	t_install_fail(m, kd, n0, b0);
	t_low_boundary(m, kd, n0, b0);
	CK(mf_flush_frozen(m, row_of, NULL) == XTC_OK);
	CK(mf_nruns(m) == n0 + 1 && mf_boundary(m) == g_bnd * 1000u &&
	    !kd_has_frozen(kd));
	for (i = 0; i < 200; i++)
		bad += !found_as(kd, 4, (int64_t)i, 30000 + i);
	CK(bad == 0);
	mf_close(m);
	kd_destroy(kd);
	bm_destroy(bm);
	printf("  retry: a failed flush leaves the delta frozen and the "
	    "manifest alone; the retry installs it\n");
}

static int
t_renamed_sync(const char *dir)
{
	if (g_fail_dirsync) {
		g_fail_dirsync = 0;
		return XTC_E_IO;
	}
	return mf_sync_dir(dir);
}

static void
t_renamed(void)
{
	manifest_t *m = NULL;
	keydir_t *kd = NULL;
	char name[64];
	uint32_t i, n0, bad = 0;
	bm_t *bm;

	bm = open_bm();
	CK(kd_create(4096, &kd) == XTC_OK);
	CK(mf_open(g_dir, bm, kd, &m) == XTC_OK);
	if (m == NULL)
		return;
	n0 = mf_nruns(m);
	for (i = 0; i < 100; i++)
		CK(kd_delta_put(kd, 5, (int64_t)i, 50000 + i) == XTC_OK);
	g_fail_dirsync = 1;
	CK(mf_flush(m, row_of, NULL, boundary, NULL) == XTC_E_IO);
	snprintf(name, sizeof name, "rl-%" PRIu64 ".run",
	    m->runs[m->n - 1u].meta.runid);
	CK(mf_nruns(m) == n0 + 1 && exists(name) && !kd_has_frozen(kd));
	CK(kd_delta_put(kd, 5, 1000, 1) == XTC_OK);
	CK(mf_flush(m, row_of, NULL, boundary, NULL) == XTC_E_IO);
	mf_close(m);
	kd_destroy(kd);
	/* The renamed manifest is the store's: it opens, with the run. */
	CK(kd_create(4096, &kd) == XTC_OK);
	CK(mf_open(g_dir, bm, kd, &m) == XTC_OK);
	if (m == NULL)
		return;
	CK(mf_nruns(m) == n0 + 1);
	for (i = 0; i < 100; i++)
		bad += !found_as(kd, 5, (int64_t)i, 50000 + i);
	CK(bad == 0);
	mf_close(m);
	kd_destroy(kd);
	bm_destroy(bm);
	printf("  renamed: a failure after the rename keeps the run, "
	    "fail-stops, and the reopen finds every row\n");
}

static void
t_text(void)
{
	manifest_t m;
	run_meta_t add, *v = NULL;
	uint64_t next = 0, bnd = 0;
	uint32_t i, n = 0, bad = 0;
	size_t len = 0;
	char *t = NULL;

	memset(&m, 0, sizeof m);
	m.n = m.cap = 64;
	CK((m.runs = calloc(m.n, sizeof *m.runs)) != NULL);
	if (m.runs == NULL)
		return;
	for (i = 0; i < m.n; i++) {
		m.runs[i].meta.runid = UINT64_MAX - 100u + i;
		m.runs[i].meta.level = UINT32_MAX;
		m.runs[i].meta.slot = 2;
		m.runs[i].meta.nrows = UINT64_MAX - i;
		m.runs[i].meta.hdr_crc = 0xFFFFFFFFu - i;
	}
	add = m.runs[0].meta;
	add.runid = UINT64_MAX - 1u;
	CK(mf_text(&m, &add, UINT64_MAX, UINT64_MAX, &t, &len) == XTC_OK);
	if (t != NULL) {
		CK(len < 2u * MF_HEAD_MAX + MF_LINE_MAX * 65u &&
		    t[len - 1] == '\n');
		CK(mf_parse(t, len, &next, &bnd, &v, &n) == XTC_OK);
		CK(n == 65 && next == UINT64_MAX && bnd == UINT64_MAX);
		for (i = 0; v != NULL && i < n && i < m.n; i++)
			bad += v[i].runid != m.runs[i].meta.runid ||
			    v[i].level != m.runs[i].meta.level ||
			    v[i].slot != m.runs[i].meta.slot ||
			    v[i].nrows != m.runs[i].meta.nrows ||
			    v[i].hdr_crc != m.runs[i].meta.hdr_crc;
		CK(bad == 0 && v != NULL && v[64].runid == add.runid);
	}
	free(v);
	free(t);
	free(m.runs);
	printf("  text: 65 maximum-width run lines encode and parse back "
	    "whole\n");
}

/* Live allocations after one flush of 2000 rows: the delta's keys and
 * maps, the run set, and the new run.  Without reclamation each flush
 * leaves its frozen delta behind, about 2000 allocations. */
static size_t
live_after_flush(manifest_t *m, keydir_t *kd, uint32_t round)
{
	size_t n = 0;
	uint32_t i;

	for (i = 0; i < 2000; i++)
		CK(kd_delta_put(kd, 7, (int64_t)i, 40000 + round) == XTC_OK);
	CK(mf_flush(m, row_of, NULL, boundary, NULL) == XTC_OK);
	xtc_alloc_audit_stats(&n, NULL);
	return n;
}

static void
t_reclaim(void)
{
	manifest_t *m = NULL;
	keydir_t *kd = NULL;
	size_t first, last = 0;
	bm_t *bm;
	uint32_t r;

	bm = open_bm();
	CK(kd_create(4096, &kd) == XTC_OK);
	CK(mf_open(g_dir, bm, kd, &m) == XTC_OK);
	if (m == NULL)
		return;
	CK(xtc_runtime_set_alloc_audit(1) == XTC_OK);
	first = live_after_flush(m, kd, 0);
	for (r = 1; r < 20; r++)
		last = live_after_flush(m, kd, r);
	CK(xtc_runtime_set_alloc_audit(0) == XTC_OK);
	/* 19 more runs (a few allocations each), no retired delta. */
	CK(last < first + 19u * 64u);
	printf("  reclaim: 20 flushes, live allocations %zu after the first, "
	    "%zu after the last\n", first, last);
	mf_close(m);
	kd_destroy(kd);
	bm_destroy(bm);
}

/* ---- concurrent commits and flushes ---- */

#define CC_FIBERS 4
#define CC_ROWS   3000
static keydir_t   *g_kd;
static manifest_t *g_m;
static uint32_t    g_newest[CC_FIBERS][CC_ROWS];
static int         g_left, g_flushes;

static void
committer(void *arg)
{
	uint32_t id = (uint32_t)(uintptr_t)arg, i, k, f;

	for (i = 0; i < 3u * CC_ROWS; i++) {
		k = (i * 7919u) % CC_ROWS;
		f = 1u + id * 1000000u + i;
		if (kd_delta_put(g_kd, 100 + id, (int64_t)k, f) == XTC_OK)
			g_newest[id][k] = f;
		if (i % 64u == 0)
			(void)xtc_proc_sleep(500000);
	}
	(void)xtc_atomic_fetch_sub_i32(&g_left, 1, xtc_mo_seq_cst);
}

static void
flusher(void *arg)
{
	(void)arg;
	while (xtc_atomic_load_i32(&g_left, xtc_mo_seq_cst) > 0) {
		if (mf_flush(g_m, row_of, NULL, boundary, NULL) == XTC_OK)
			g_flushes++;
		(void)xtc_proc_sleep(100000);
	}
}

static void
t_concurrent(void)
{
	xtc_exec_t *ex = NULL;
	xtc_proc_opts_t po = XTC_PROC_OPTS_INIT;
	xtc_pid_t pid;
	bm_t *bm;
	uint32_t f, i, k, bad = 0;

	bm = open_bm();
	CK(kd_create(4096, &g_kd) == XTC_OK);
	CK(mf_open(g_dir, bm, g_kd, &g_m) == XTC_OK);
	if (g_m == NULL)
		return;
	CK(xtc_exec_create(&(xtc_exec_opts_t){ .size = sizeof(xtc_exec_opts_t),
	    .n_loops = 2 }, &ex) == XTC_OK);
	if (ex == NULL)
		return;
	xtc_atomic_store_i32(&g_left, CC_FIBERS, xtc_mo_seq_cst);
	for (i = 0; i < CC_FIBERS; i++)
		CK(xtc_proc_spawn(xtc_exec_loop(ex, (int)(i % 2u)), committer,
		    (void *)(uintptr_t)i, &po, &pid) == XTC_OK);
	CK(xtc_proc_spawn(xtc_exec_loop(ex, 1), flusher, NULL, &po, &pid) ==
	    XTC_OK);
	CK(xtc_exec_run(ex) == XTC_OK);
	xtc_exec_destroy(ex);
	/* Each RID's newest frame is in the delta, or the newest run reads
	 * it (a resident map is not used here: frames are "evicted"). */
	xtc_rcu_read_lock();
	for (i = 0; i < CC_FIBERS; i++)
		for (k = 0; k < CC_ROWS; k++) {
			if (kd_delta_get(g_kd, 100 + i, (int64_t)k, &f)) {
				bad += f != g_newest[i][k];
				continue;
			}
			xtc_rcu_read_unlock();
			bad += !found_as(g_kd, 100 + i, (int64_t)k,
			    g_newest[i][k]);
			xtc_rcu_read_lock();
		}
	xtc_rcu_read_unlock();
	CK(bad == 0);
	CK(g_flushes > 2);
	printf("  concurrent: %d committers on 2 loops, %d flushes, every RID "
	    "at its newest place\n", CC_FIBERS, g_flushes);
	mf_close(g_m);
	kd_destroy(g_kd);
	bm_destroy(bm);
}

int
main(void)
{
	xtc_fs_dir_t *d;
	const char *name;
	char p[300];

	t_tmpl(g_dir, sizeof g_dir, "manifest");
	if (mkdtemp(g_dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	t_flush_reopen();
	t_refuse();
	t_missing();
	t_retry();
	t_renamed();
	t_text();
	t_reclaim();
	t_concurrent();
	if (xtc_fs_dir_open(g_dir, &d) == XTC_OK) {
		while (xtc_fs_dir_next(d, &name) == XTC_OK && name != NULL) {
			snprintf(p, sizeof p, "%s/%s", g_dir, name);
			(void)unlink(p);
		}
		xtc_fs_dir_close(d);
	}
	(void)rmdir(g_dir);
	xtc_rcu_fini();
	if (!g_fail)
		printf("ok test_manifest\n");
	return g_fail;
}
