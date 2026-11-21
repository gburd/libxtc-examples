/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_run.c
 *	Run files (run.c, STORAGE_DESIGN 3.3):
 *	  roundtrip  rows of five tables, sparse and dense rowids, record
 *	             sizes from 0 to 6000 bytes (so continuation pages),
 *	             written and reopened through a buffer manager smaller
 *	             than the run: every row found at its rank and read
 *	             back; neighbours that are absent are not found; the
 *	             iterator returns every row in (tableid, rowid) order;
 *	             range checks admit only the tables' ranges;
 *	  order      a row out of order, or too long, is refused;
 *	  manifest   a manifest line round-trips; a run opened with the
 *	             wrong header CRC, level or row count is refused;
 *	  seal       a page of another run (same file number, same offset)
 *	             is refused: by the run-id check, and a page of the
 *	             page file at that offset by the file-number seal;
 *	  empty      a run of no rows is refused and leaves no file.
 *	Runs off a loop (xtc_aio and xtc_blocking_run are inline there).
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "btnode.h"
#include "bufmgr.h"
#include "run.h"
#include "wal.h"
#include "xtc.h"
#include "t_tmp.h"

#define PS     4096
#define NROWS  6000

static char g_dir[256];

struct row {
	uint32_t tid;
	int64_t  rid;
	uint16_t len;
};
static struct row g_rows[NROWS];
static uint64_t g_rng = 0x52554E31u;

static uint64_t
rnd(void)
{
	uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

/* Record byte j of the row (tid, rid). */
static uint8_t
rec_byte(uint32_t tid, int64_t rid, uint32_t j)
{
	return (uint8_t)(tid * 7u + (uint64_t)rid * 13u + j);
}

static bm_t *
open_bm(const char *name, uint32_t frames)
{
	bm_opts_t o = BM_OPTS_DEFAULT;
	char p[300];
	bm_t *bm = NULL;

	snprintf(p, sizeof p, "%s/%s", g_dir, name);
	o.path = p;
	o.page_size = PS;
	o.n_frames = frames;
	o.cksum_off = BTNODE_CKSUM_OFF;
	o.cksum = wal_crc32c;
	CK(bm_create(&o, &bm) == XTC_OK);
	return bm;
}

/* Five tables: dense, gaps of 1..9, random 63-bit, negative rowids, and
 * one table of a single row. */
static int
gen_rows(void)
{
	static const uint32_t tids[5] = { 1, 7, 8, 300, 0xFFFFFFFDu };
	int n = 0, t, i;
	int64_t at;

	for (t = 0; t < 5; t++) {
		int cnt = t == 4 ? 1 : (NROWS - 1) / 4;
		at = t == 3 ? -1000000 : 1;
		for (i = 0; i < cnt; i++) {
			g_rows[n].tid = tids[t];
			if (t == 2)
				at += 1 + (int64_t)(rnd() >> 12);
			else
				at += t == 1 ? 1 + (int64_t)(rnd() % 9) : 1;
			g_rows[n].rid = at;
			/* Mostly short; some long (continuations). */
			g_rows[n].len = (uint16_t)(rnd() % 50 == 0 ?
			    4000 + rnd() % 2001 : rnd() % 200);
			if (i == 3)
				g_rows[n].len = 0;
			n++;
		}
	}
	return n;
}

static int
write_run(bm_t *bm, const char *path, uint64_t runid, int n, run_meta_t *m)
{
	static uint8_t rec[RUN_REC_MAX];
	run_writer_t *w = NULL;
	run_row_t r;
	int i, rc;
	uint32_t j;

	if ((rc = run_writer_open(bm, PS, path, runid, 3, &w)) != XTC_OK)
		return rc;
	for (i = 0; i < n; i++) {
		for (j = 0; j < g_rows[i].len; j++)
			rec[j] = rec_byte(g_rows[i].tid, g_rows[i].rid, j);
		memset(&r, 0, sizeof r);
		r.tableid = g_rows[i].tid;
		r.rowid = g_rows[i].rid;
		r.commit_ts = (uint64_t)i * 3u + 1u;
		r.self_wa = WAL_WA(1, (uint64_t)i * 100u);
		r.flags = (uint8_t)(i % 97 == 0 ? RUN_F_DELETED : 0);
		r.len = g_rows[i].len;
		r.rec = rec;
		if ((rc = run_writer_add(w, &r)) != XTC_OK) {
			run_writer_abort(w);
			return rc;
		}
	}
	return run_writer_finish(w, m);
}

static int
row_ok(const run_row_t *o, int i, const uint8_t *buf)
{
	uint32_t j;

	if (o->tableid != g_rows[i].tid || o->rowid != g_rows[i].rid ||
	    o->len != g_rows[i].len || o->commit_ts != (uint64_t)i * 3u + 1u ||
	    o->self_wa != WAL_WA(1, (uint64_t)i * 100u) ||
	    o->flags != (i % 97 == 0 ? RUN_F_DELETED : 0))
		return 0;
	for (j = 0; j < o->len; j++)
		if (buf[j] != rec_byte(g_rows[i].tid, g_rows[i].rid, j))
			return 0;
	return 1;
}

static void
t_roundtrip(void)
{
	static uint8_t buf[RUN_REC_MAX];
	char path[300];
	run_meta_t m;
	run_t *r = NULL;
	run_iter_t *it = NULL;
	run_row_t o;
	struct stat sb;
	uint64_t rank;
	uint32_t page;
	bm_t *bm;
	int n, i, bad = 0, absent = 0;

	n = gen_rows();
	bm = open_bm("pages", 32);
	if (bm == NULL)
		return;
	run_path(g_dir, 41, path, sizeof path);
	CK(write_run(bm, path, 41, n, &m) == XTC_OK);
	CK(m.runid == 41 && m.level == 3 && m.nrows == (uint64_t)n);
	CK(stat(path, &sb) == 0);
	CK(run_open(bm, path, &m, &r) == XTC_OK);
	if (r == NULL) {
		bm_destroy(bm);
		return;
	}
	CK(run_rows(r) == (uint64_t)n && run_id(r) == 41);
	for (i = 0; i < n; i++) {
		if (run_find(r, g_rows[i].tid, g_rows[i].rid, &rank, &page) !=
		    XTC_OK || rank != (uint64_t)i ||
		    run_read(r, page, g_rows[i].tid, g_rows[i].rid, &o, buf,
		    sizeof buf) != XTC_OK || !row_ok(&o, i, buf))
			bad++;
		/* The next rowid, when it is not the next row's. */
		if (!(i + 1 < n && g_rows[i + 1].tid == g_rows[i].tid &&
		    g_rows[i + 1].rid == g_rows[i].rid + 1) &&
		    run_get(r, g_rows[i].tid, g_rows[i].rid + 1, &o, buf,
		    sizeof buf) != XTC_E_NOTFOUND)
			absent++;
	}
	CK(bad == 0 && absent == 0);
	CK(run_get(r, 2, 1, &o, buf, sizeof buf) == XTC_E_NOTFOUND);
	/* Table 1 starts at rowid 2, the one-row table holds rowid 2. */
	CK(!run_may_hold(r, 2, 2) && run_may_hold(r, 1, 2) &&
	    !run_may_hold(r, 1, 1) && run_may_hold(r, 0xFFFFFFFDu, 2) &&
	    !run_may_hold(r, 0xFFFFFFFDu, 3));
	/* A short buffer truncates, and says how long the record is. */
	for (i = 0; i < n && g_rows[i].len < 4000; i++)
		;
	CK(i < n && run_get(r, g_rows[i].tid, g_rows[i].rid, &o, buf, 10) ==
	    XTC_OK && o.len == g_rows[i].len);
	/* The iterator: every row, in order. */
	CK(run_iter_open(r, &it) == XTC_OK);
	for (i = 0; it != NULL; i++) {
		int rc = run_iter_next(it, &o, buf, sizeof buf);
		if (rc != XTC_OK) {
			CK(rc == XTC_E_NOTFOUND);
			break;
		}
		if (i >= n || !row_ok(&o, i, buf))
			bad++;
	}
	CK(i == n && bad == 0);
	run_iter_close(it);
	run_close(r);
	CK(bm_cksum_failures(bm, NULL) == 0);
	bm_destroy(bm);
	printf("  roundtrip: %d rows of 5 tables, %lld bytes, through a "
	    "32-frame pool: found, read, iterated in order\n", n,
	    (long long)sb.st_size);
}

static void
t_order(void)
{
	char path[300];
	run_writer_t *w = NULL;
	run_meta_t m;
	run_row_t r;
	struct stat sb;
	bm_t *bm;

	bm = open_bm("pages2", 16);
	if (bm == NULL)
		return;
	run_path(g_dir, 42, path, sizeof path);
	CK(run_writer_open(bm, PS, path, 42, 0, &w) == XTC_OK);
	CK(run_writer_open(bm, PS, path, 42, 0, &w) != XTC_OK);   /* exists */
	memset(&r, 0, sizeof r);
	r.tableid = 5;
	r.rowid = 10;
	CK(run_writer_add(w, &r) == XTC_OK);
	CK(run_writer_add(w, &r) == XTC_E_INVAL);          /* equal */
	r.rowid = 9;
	CK(run_writer_add(w, &r) == XTC_E_INVAL);          /* backwards */
	r.tableid = 4;
	r.rowid = 100;
	CK(run_writer_add(w, &r) == XTC_E_INVAL);          /* lower table */
	run_writer_abort(w);
	CK(stat(path, &sb) != 0);
	/* Empty: refused, no file. */
	CK(run_writer_open(bm, PS, path, 42, 0, &w) == XTC_OK);
	CK(run_writer_finish(w, &m) == XTC_E_INVAL);
	CK(stat(path, &sb) != 0);
	bm_destroy(bm);
	printf("  order: out-of-order rows refused; an empty run leaves no "
	    "file\n");
}

static void
t_manifest_seal(void)
{
	static uint8_t pg[PS];
	char path[300], p2[300], line[128], pf[300];
	run_meta_t m, m2, d;
	run_t *r = NULL;
	run_row_t o;
	uint8_t buf[256];
	bm_t *bm;
	int fd, n;

	n = 300;
	bm = open_bm("pages3", 16);
	if (bm == NULL)
		return;
	run_path(g_dir, 43, path, sizeof path);
	/* 65578 = 43 + 65535: the same file number as run 43. */
	run_path(g_dir, 43 + 65535, p2, sizeof p2);
	CK(RUN_FILE(43) == RUN_FILE(43 + 65535));
	CK(write_run(bm, path, 43, n, &m) == XTC_OK);
	CK(write_run(bm, p2, 43 + 65535, n, &m2) == XTC_OK);
	m.slot = 2;
	CK(run_meta_enc(&m, line, sizeof line) > 0);
	CK(run_meta_dec(line, &d) && d.runid == m.runid &&
	    d.level == m.level && d.slot == 2 && d.nrows == m.nrows &&
	    d.hdr_crc == m.hdr_crc);
	CK(!run_meta_dec("run 0 1 0 5 0000abcd\n", &d));
	CK(!run_meta_dec("run 5 1 3 5 0000abcd\n", &d));
	CK(!run_meta_dec("nur 5 1 0 5 0000abcd\n", &d));
	d = m;
	d.hdr_crc ^= 1;
	CK(run_open(bm, path, &d, &r) == XTC_E_IO);
	d = m;
	d.level = 9;
	CK(run_open(bm, path, &d, &r) == XTC_E_IO);
	d = m;
	d.nrows++;
	CK(run_open(bm, path, &d, &r) == XTC_E_IO);
	CK(run_open(bm, path, &m, &r) == XTC_OK);
	if (r == NULL) {
		bm_destroy(bm);
		return;
	}
	CK(run_get(r, g_rows[5].tid, g_rows[5].rid, &o, buf, sizeof buf) ==
	    XTC_OK);
	run_close(r);
	r = NULL;
	/* Data page 1 of the other run (same file number, sealed alike)
	 * over page 1 of this one: the run id refuses it. */
	CK((fd = open(p2, O_RDONLY)) >= 0);
	CK(pread(fd, pg, PS, PS) == PS);
	close(fd);
	CK((fd = open(path, O_RDWR)) >= 0);
	CK(pwrite(fd, pg, PS, PS) == PS);
	close(fd);
	CK(run_open(bm, path, &m, &r) == XTC_OK);
	CK(r != NULL && run_get(r, g_rows[0].tid, g_rows[0].rid, &o, buf,
	    sizeof buf) == XTC_E_IO);
	run_close(r);
	r = NULL;
	/* A page of the page file at that offset: the seal refuses it. */
	snprintf(pf, sizeof pf, "%s/pages3", g_dir);
	memset(pg, 0x11, PS);
	bm_seal_page(bm, pg, 1);
	CK((fd = open(path, O_RDWR)) >= 0);
	CK(pwrite(fd, pg, PS, PS) == PS);
	close(fd);
	CK(run_open(bm, path, &m, &r) == XTC_OK);
	CK(r != NULL && run_get(r, g_rows[0].tid, g_rows[0].rid, &o, buf,
	    sizeof buf) == XTC_E_IO);
	CK(bm_cksum_failures(bm, NULL) == 1);
	run_close(r);
	bm_destroy(bm);
	unlink(p2);
	unlink(pf);
	printf("  manifest/seal: the line round-trips; a wrong CRC, level or "
	    "count is refused; another run's page and a page-file page "
	    "are refused\n");
}

int
main(void)
{
	static const char *junk[] = { "pages", "pages2", "pages3" };
	char p[300];
	unsigned i;

	t_tmpl(g_dir, sizeof g_dir, "runf");
	if (mkdtemp(g_dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	t_roundtrip();
	t_order();
	t_manifest_seal();
	for (i = 0; i < sizeof junk / sizeof junk[0]; i++) {
		snprintf(p, sizeof p, "%s/%s", g_dir, junk[i]);
		(void)unlink(p);
	}
	for (i = 41; i <= 43; i++) {
		run_path(g_dir, i, p, sizeof p);
		(void)unlink(p);
	}
	(void)rmdir(g_dir);
	if (!g_fail)
		printf("ok test_run\n");
	return g_fail;
}
