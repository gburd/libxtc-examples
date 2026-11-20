/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/bench_point_iscan.c
 *	The cost of an indexed point read, the path an equality index scan
 *	open takes.  One table t(k INTEGER PRIMARY KEY, u INT, e TEXT,
 *	pad TEXT) with a unique index on u and one on e, every page
 *	resident.  Each cell runs random point reads and prints ns per
 *	read:
 *	  iscan  xstore_iscan_open on u = ?, one xstore_iscan_next, close
 *	         (the engine's index-scan seam alone, no SQL);
 *	  sql_u  SELECT pad FROM t WHERE u = ? (prepared once, rebound);
 *	  sql_e  SELECT pad FROM t WHERE e = ? (a 30-byte TEXT key);
 *	  sql_k  SELECT pad FROM t WHERE k = ? (the rowid point: the
 *	         control, which no index scan reaches).
 *	For A/B, build the binary at each commit and interleave the runs
 *	pinned to one CPU (taskset); compare medians.
 *
 *	usage: bench_point_iscan [rows] [reads]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xtc.h"
#include "bufmgr.h"
#include "btree.h"
#include "engine.h"
#include "xstore.h"
#include "t_tmp.h"

/* The 30-byte TEXT key of row i. */
static void
ekey(char *out, long i)
{
	snprintf(out, 31, "email-%012ld@example.org", i);
}

/* Load `rows` rows and build both indexes.  0, or -1 on any failure. */
static int
load(sx_db *db, long rows)
{
	sx_stmt *st = NULL;
	char e[32], pad[64];
	long i;

	if (sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, u INT, "
	    "e TEXT, pad TEXT)", NULL) != SX_OK)
		return -1;
	if (sx_exec(db, "BEGIN", NULL) != SX_OK ||
	    sx_prepare(db, "INSERT INTO t VALUES(?, ?, ?, ?)", -1, &st, NULL) !=
	    SX_OK)
		return -1;
	memset(pad, 'p', sizeof pad - 1);
	pad[sizeof pad - 1] = '\0';
	for (i = 1; i <= rows; i++) {
		ekey(e, i);
		sx_bind_int64(st, 1, i);
		sx_bind_int64(st, 2, i * 7);
		sx_bind_text(st, 3, e, -1);
		sx_bind_text(st, 4, pad, -1);
		if (sx_step(st) != SX_DONE)
			return -1;
		sx_reset(st);
	}
	sx_finalize(st);
	if (sx_exec(db, "COMMIT", NULL) != SX_OK ||
	    sx_exec(db, "CREATE UNIQUE INDEX t_u ON t(u)", NULL) != SX_OK ||
	    sx_exec(db, "CREATE INDEX t_e ON t(e)", NULL) != SX_OK)
		return -1;
	return 0;
}

/* xorshift64: the key sequence, the same in every run. */
static uint64_t
rnd(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

/* `reads` iscan opens on u = random existing value; ns per read, or -1
 * if a read did not find exactly its row. */
static double
cell_iscan(sx_db *db, bt_t *bt, long rows, long reads)
{
	uint32_t idx = 0;
	uint64_t s = 88172645463325252ull;
	int64_t t0, rid;
	const uint8_t *rec;
	int rl;
	long i;

	if (!xstore_index_lookup(bt, "t", "u", &idx))
		return -1;
	t0 = xtc_clock_mono();
	for (i = 0; i < reads; i++) {
		long r = (long)(rnd(&s) % (uint64_t)rows) + 1;
		xstore_val_t v;
		xstore_iscan_t *sc;

		memset(&v, 0, sizeof v);
		v.cls = XSTORE_C_INT;
		v.i = r * 7;
		sc = xstore_iscan_open((struct xsql *)db, "t", idx, &v, 1, NULL,
		    0, NULL, 0, 0);
		if (sc == NULL || xstore_iscan_next(sc, &rid, &rec, &rl) != 1 ||
		    rid != r || xstore_iscan_next(sc, &rid, &rec, &rl) != 0)
			return -1;
		xstore_iscan_close(sc);
	}
	return (double)(xtc_clock_mono() - t0) / (double)reads;
}

/* `reads` executions of `sql` with one random parameter (`text`: the
 * row's e key, else the int `mul` * row); ns per read, or -1 if one did
 * not return exactly one row. */
static double
cell_sql(sx_db *db, const char *sql, long rows, long reads, int text,
    int mul)
{
	sx_stmt *st = NULL;
	uint64_t s = 88172645463325252ull;
	int64_t t0;
	char e[32];
	long i;

	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK)
		return -1;
	t0 = xtc_clock_mono();
	for (i = 0; i < reads; i++) {
		long r = (long)(rnd(&s) % (uint64_t)rows) + 1;
		int n = 0;

		if (text) {
			ekey(e, r);
			sx_bind_text(st, 1, e, -1);
		} else {
			sx_bind_int64(st, 1, r * mul);
		}
		while (sx_step(st) == SX_ROW)
			n++;
		sx_reset(st);
		if (n != 1) {
			sx_finalize(st);
			return -1;
		}
	}
	sx_finalize(st);
	return (double)(xtc_clock_mono() - t0) / (double)reads;
}

int
main(int argc, char **argv)
{
	long rows = argc > 1 ? atol(argv[1]) : 200000;
	long reads = argc > 2 ? atol(argv[2]) : 400000;
	char path[256];
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL;
	double c[4];
	int fd;

	t_tmpl(path, sizeof path, "bench-point-iscan");
	fd = mkstemp(path);
	if (fd < 0)
		return 1;
	close(fd);
	bo.path = path;
	bo.page_size = 4096;
	bo.n_frames = 65536; /* 256 MiB: every page resident */
	if (sx_init() != SX_OK || bm_create(&bo, &bm) != XTC_OK ||
	    bt_open(bm, &bt) != XTC_OK || sx_open_bt(bt, &db) != SX_OK ||
	    load(db, rows) != 0) {
		fprintf(stderr, "setup failed\n");
		unlink(path);
		return 1;
	}
	/* Warm every page and the plan caches once, then measure. */
	(void)cell_sql(db, "SELECT pad FROM t WHERE u = ?", rows, rows, 0, 7);
	(void)cell_sql(db, "SELECT pad FROM t WHERE e = ?", rows, rows, 1, 0);
	c[0] = cell_iscan(db, bt, rows, reads);
	c[1] = cell_sql(db, "SELECT pad FROM t WHERE u = ?", rows, reads, 0, 7);
	c[2] = cell_sql(db, "SELECT pad FROM t WHERE e = ?", rows, reads, 1, 0);
	c[3] = cell_sql(db, "SELECT pad FROM t WHERE k = ?", rows, reads, 0, 1);
	printf("rows=%ld reads=%ld iscan=%.1f sql_u=%.1f sql_e=%.1f "
	    "sql_k=%.1f ns\n", rows, reads, c[0], c[1], c[2], c[3]);
	sx_close(db);
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);
	return c[0] < 0 || c[1] < 0 || c[2] < 0 || c[3] < 0;
}
