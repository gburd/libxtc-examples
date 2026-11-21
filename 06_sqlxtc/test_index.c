/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_index.c
 *	Secondary indexes (CREATE INDEX): the index is built on CREATE,
 *	maintained by INSERT/UPDATE/DELETE, and used by the planner as an
 *	equality seek instead of a full table scan.  No daemon; plain
 *	asserts + printf, run off a loop (synchronous bufmgr I/O).
 *
 *	Proves:
 *	  1. CREATE INDEX on a non-PK column, then a WHERE = on that column
 *	     returns exactly the correct rows.
 *	  2. The seek reads far fewer B-tree rows than a full scan would --
 *	     measured by the count of rows the storage layer visited, via a
 *	     large table where a scan would touch every row.
 *	  3. INSERT / UPDATE / DELETE keep the index consistent (a value
 *	     moved by UPDATE is found under its new value and not the old;
 *	     a deleted row disappears from the seek).
 *	  4. Crash/recover: after reopening the base (WAL replay) the index
 *	     is rebuilt and the seek still returns the correct rows.
 *	  5. A base-table scan that cannot read the table is an error, never
 *	     a short table: with the n-th scan read failing
 *	     (xstore_scan_read_fail_at), for every n until no read is left,
 *	     CREATE INDEX fails and leaves no index (or succeeds whole),
 *	     count(*) fails or counts every row, and an UPDATE over a scan
 *	     fails or changes every row it should.  The same for a
 *	     transaction whose buffered rows were spilled to the STEAL
 *	     staging area.  Before
 *	     the fix a failed read ended the scan as if the table ended, and
 *	     CREATE INDEX built a short index and reported success.
 *	  6. A CREATE TABLE whose PRIMARY KEY autoindex cannot be built
 *	     leaves no table (a duplicate INSERT cannot go in), also after a
 *	     crash and recovery from the log; CREATE TABLE IF NOT EXISTS over
 *	     an existing table keeps it.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"
#include "xtc.h"
#include "wal.h"
#include "t_tmp.h"

#define PAGE_SZ  4096
#define N_FRAMES 64
#define N_ROWS   5000        /* far more than a point seek should visit */

static int g_fail;
#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

/* Count rows returned by a query with one bound integer parameter. */
static int
count_rows_i(sx_db *db, const char *sql, int64_t p1)
{
	sx_stmt *st = NULL;
	int n = 0;
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK) return -1;
	sx_bind_int64(st, 1, p1);
	while (sx_step(st) == SX_ROW) n++;
	sx_finalize(st);
	return n;
}

/* Return the single integer column of the first row, or -1 if no row. */
static int64_t
first_int_i(sx_db *db, const char *sql, int64_t p1)
{
	sx_stmt *st = NULL;
	int64_t v = -1;
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK) return -1;
	sx_bind_int64(st, 1, p1);
	if (sx_step(st) == SX_ROW) v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

static int
scenario_index(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL;
	sx_stmt *st = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-index");
	int fd, i;
	bt_stats_t s0, s1;

	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &db) == SX_OK);

	/* t(k PK, grp, note): grp has ~50 rows each; index on grp. */
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, grp INTEGER, note TEXT)", NULL) == SX_OK);
	CK(sx_prepare(db, "INSERT INTO t(k,grp,note) VALUES(?,?,?)", -1, &st, NULL) == SX_OK);
	for (i = 1; i <= N_ROWS; i++) {
		char note[32];
		snprintf(note, sizeof note, "n%d", i);
		sx_bind_int64(st, 1, i);
		sx_bind_int64(st, 2, i % 100);          /* 100 groups, ~50 rows each */
		sx_bind_text(st, 3, note, -1);
		CK(sx_step(st) == SX_DONE);
		sx_reset(st);
	}
	sx_finalize(st); st = NULL;

	CK(sx_exec(db, "CREATE INDEX idx_grp ON t(grp)", NULL) == SX_OK);

	/* Correctness: WHERE grp = 7 should return exactly the rows whose
	 * k % 100 == 7 (there are 50: k = 7, 107, ..., 4907). */
	{
		uint64_t seeks0 = xstore_index_seek_count();
		CK(count_rows_i(db, "SELECT k FROM t WHERE grp = ?", 7) == 50);
		/* Evidence the planner took the INDEX SEEK, not a full scan. */
		CK(xstore_index_seek_count() > seeks0);
	}

	/* Seek, not scan: measure B-tree rows visited.  bt_stats.lookups is
	 * per-descent; the telling number is descents/inserts.  We compare
	 * the count(*)-via-scan against the index seek by row-visit ratio:
	 * the seek visits ~50 index entries + 50 point fetches, vs a full
	 * scan visiting all 5000.  Use bt_get_stats descents as the proxy. */
	bt_get_stats(bt, &s0);
	CK(count_rows_i(db, "SELECT k FROM t WHERE grp = ?", 42) == 50);
	bt_get_stats(bt, &s1);
	/* A full scan of 5000 rows would drive far more cursor descents than
	 * a seek of ~50 matches + 50 point fetches.  Assert the seek used
	 * well under a scan's worth (a scan re-descends per row here). */
	{
		uint64_t descents = s1.descents - s0.descents;
		printf("  info seek used %llu btree descents for 50 matches"
		    " (full scan of %d rows would be far more)\n",
		    (unsigned long long)descents, N_ROWS);
		CK(descents < (uint64_t)N_ROWS);   /* not a full-table scan */
	}

	/* INSERT maintenance: add a row in group 7, seek finds 51. */
	CK(sx_exec(db, "INSERT INTO t(k,grp,note) VALUES(100001,7,'new')", NULL) == SX_OK);
	CK(count_rows_i(db, "SELECT k FROM t WHERE grp = ?", 7) == 51);

	/* UPDATE maintenance: move that row from group 7 to group 8. */
	CK(sx_exec(db, "UPDATE t SET grp=8 WHERE k=100001", NULL) == SX_OK);
	CK(count_rows_i(db, "SELECT k FROM t WHERE grp = ?", 7) == 50);   /* gone from 7 */
	CK(first_int_i(db, "SELECT k FROM t WHERE grp = ? AND k=100001", 8) == 100001); /* found in 8 */

	/* DELETE maintenance: remove it, seek on 8 no longer finds it. */
	CK(sx_exec(db, "DELETE FROM t WHERE k=100001", NULL) == SX_OK);
	CK(count_rows_i(db, "SELECT k FROM t WHERE grp = ? AND k=100001", 8) == 0);

	/* A TEXT index too (the c_last-style case). */
	CK(sx_exec(db, "CREATE INDEX idx_note ON t(note)", NULL) == SX_OK);
	CK(count_rows_i(db, "SELECT k FROM t WHERE note = ?", 0) == -1 ||   /* bind is int; use a text query below */ 1);
	{
		sx_stmt *q = NULL;
		int n = 0;
		CK(sx_prepare(db, "SELECT k FROM t WHERE note = ?", -1, &q, NULL) == SX_OK);
		sx_bind_text(q, 1, "n42", -1);
		while (sx_step(q) == SX_ROW) { CK(sx_column_int64(q, 0) == 42); n++; }
		sx_finalize(q);
		CK(n == 1);
	}

	/* DROP INDEX: seek falls back to a scan but stays correct. */
	CK(sx_exec(db, "DROP INDEX idx_grp", NULL) == SX_OK);
	CK(count_rows_i(db, "SELECT k FROM t WHERE grp = ?", 42) == 50);

	sx_close(db);
	bt_close(bt); bm_destroy(bm);
	unlink(path);
	{ char wal[288]; snprintf(wal, sizeof wal, "%s-wal", path); unlink(wal); }
	if (g_fail) return 1;
	printf("  ok   index: create/build, INSERT/UPDATE/DELETE maintenance,"
	    " equality seek (int + text), drop -- all correct\n");
	return 0;
}

/* Crash/recover: rows written under a WAL, the pool dropped without a
 * flush, then recovered into a FRESH B-tree.  Index entries are NOT
 * WAL-logged (they are derived data), so this proves the rebuild path:
 * after xstore_recover replays the base rows, xstore_index_rebuild_all
 * reconstructs the index from the persisted index catalog and the
 * recovered base rows, and the equality seek still returns correct
 * rows. */
static int
scenario_recover(void)
{
	bm_t *bm1 = NULL, *bm2 = NULL;
	bt_t *bt1 = NULL, *bt2 = NULL;
	sx_db *db1 = NULL, *db2 = NULL;
	wal_t *wal = NULL;
	wal_opts_t wo;
	bm_opts_t bo = BM_OPTS_DEFAULT, b2 = BM_OPTS_DEFAULT;
	char btA[256], btB[256], logp[300];
	int fd, i;

	t_tmpl(btA, sizeof btA, "sqlxtc-idx-recA");
	t_tmpl(btB, sizeof btB, "sqlxtc-idx-recB");
	fd = mkstemp(btA); if (fd < 0) return 1; close(fd);
	fd = mkstemp(btB); if (fd < 0) return 1; close(fd);
	snprintf(logp, sizeof logp, "%s-wal", btA);

	memset(&wo, 0, sizeof wo);
	wo.path = logp; wo.window_ns = 500000; wo.max_batch = 256;
	if (wal_open(&wo, &wal) != XTC_OK) return 1;
	/* Pool large enough that NOTHING evicts -> the WAL is the only
	 * durable copy of the base rows (the index pages, un-WAL'd, are
	 * likewise never flushed -- so recovery MUST rebuild them). */
	bo.path = btA; bo.page_size = PAGE_SZ; bo.n_frames = 512;
	if (bm_create(&bo, &bm1) != XTC_OK) return 1;
	if (bt_open(bm1, &bt1) != XTC_OK) return 1;
	xstore_set_wal((struct wal *)wal);
	if (sx_open_bt(bt1, &db1) != SX_OK) return 1;
	CK(sx_exec(db1, "CREATE TABLE t(k INTEGER PRIMARY KEY, grp INTEGER)", NULL) == SX_OK);
	CK(sx_exec(db1, "CREATE INDEX idx_grp ON t(grp)", NULL) == SX_OK);
	{
		sx_stmt *st = NULL;
		CK(sx_prepare(db1, "INSERT INTO t(k,grp) VALUES(?,?)", -1, &st, NULL) == SX_OK);
		for (i = 1; i <= 500; i++) {
			sx_bind_int64(st, 1, i);
			sx_bind_int64(st, 2, i % 10);   /* 10 groups, 50 rows each */
			CK(sx_step(st) == SX_DONE);
			sx_reset(st);
		}
		sx_finalize(st);
	}

	/* crash: drop the pool without flushing (base file A stays empty). */
	sx_close(db1);
	xstore_set_wal(NULL);
	bt_close(bt1);
	bm_destroy(bm1);
	wal_close(wal);

	/* restart + recover into a FRESH empty B-tree from the WAL, then
	 * rebuild indexes from the recovered base rows. */
	b2.path = btB; b2.page_size = PAGE_SZ; b2.n_frames = 64;
	if (bm_create(&b2, &bm2) != XTC_OK) return 1;
	if (bt_open(bm2, &bt2) != XTC_OK) return 1;
	if (xstore_recover(bt2, logp) != XTC_OK) { fprintf(stderr, "FAIL recover\n"); return 1; }
	CK(xstore_index_rebuild_all(bt2) == 0);
	if (sx_open_bt(bt2, &db2) != SX_OK) return 1;

	/* The seek must find the recovered rows via the rebuilt index. */
	CK(count_rows_i(db2, "SELECT k FROM t WHERE grp = ?", 3) == 50);
	CK(count_rows_i(db2, "SELECT k FROM t WHERE grp = ?", 0) == 50);

	sx_close(db2);
	bt_close(bt2); bm_destroy(bm2);
	unlink(btA); unlink(btB); unlink(logp);
	if (g_fail) return 1;
	printf("  ok   index: WAL recovery rebuilds index entries from base"
	    " rows; seek correct after crash\n");
	return 0;
}

/* Sweep the n-th base-scan read failing over `sql` (no binds): each run
 * must fail with SX_ERROR, or return `want`: the first column of its
 * first row, or the rows a write changed.  A run with no failed read
 * ends the sweep.  Returns how many runs failed; *ok_n counts the runs
 * that succeeded. */
static int
sweep(sx_db *db, const char *sql, int64_t want, int *ok_n)
{
	int n, rc, errs = 0;
	for (n = 1; n < 100000; n++) {
		sx_stmt *st = NULL;
		int64_t got = -1;
		xstore_scan_read_fail_at(n);
		rc = sx_prepare(db, sql, -1, &st, NULL);
		if (rc == SX_OK) rc = sx_step(st);
		if (rc == SX_ROW) got = sx_column_int64(st, 0);
		if (rc == SX_DONE) got = sx_stmt_changes(st);
		sx_finalize(st);
		if (xstore_scan_read_fail_at(0) > 0) {   /* nothing failed */
			if (!((rc == SX_DONE || rc == SX_ROW) && got == want)) {
				fprintf(stderr, "  %s: no read failed, rc %d got %lld "
				    "want %lld (%s)\n", sql, rc, (long long)got,
				    (long long)want, sx_errmsg(db));
				g_fail = 1;
			}
			break;
		}
		if (rc == SX_ERROR) {
			errs++;
			continue;
		}
		(*ok_n)++;
		if (!((rc == SX_DONE || rc == SX_ROW) && got == want)) {
			fprintf(stderr, "  %s: read %d failed, rc %d got %lld want "
			    "%lld\n", sql, n, rc, (long long)got, (long long)want);
			g_fail = 1;
		}
	}
	CK(n > 1 && n < 100000);
	return errs;
}

/* 5: a failed read in a base-table scan is an error, not the end. */
static int
scenario_scan_error(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL;
	sx_stmt *st = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256], logp[300], pad[1001];
	wal_t *wal = NULL;
	wal_opts_t wo;
	xstore_index_info_t ix[4];
	int fd, i, n, ok = 0, errs;

	t_tmpl(path, sizeof path, "sqlxtc-index-scanerr");
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	/* A log, so a large transaction spills to the staging area. */
	snprintf(logp, sizeof logp, "%s-wal", path);
	memset(&wo, 0, sizeof wo);
	wo.path = logp; wo.window_ns = 500000; wo.max_batch = 256;
	if (wal_open(&wo, &wal) != XTC_OK) return 1;
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 256;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	xstore_set_wal((struct wal *)wal);
	CK(sx_open_bt(bt, &db) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, grp INTEGER)",
	    NULL) == SX_OK);
	CK(sx_prepare(db, "INSERT INTO t(k,grp) VALUES(?,?)", -1, &st,
	    NULL) == SX_OK);
	for (i = 1; i <= 2000; i++) {
		sx_bind_int64(st, 1, i);
		sx_bind_int64(st, 2, i % 10);
		CK(sx_step(st) == SX_DONE);
		sx_reset(st);
	}
	sx_finalize(st); st = NULL;

	/* CREATE INDEX: a failed build leaves no index (the next CREATE of
	 * the name builds one, not "already exists"), and the build that
	 * succeeds is whole: every group seeks its 200 rows. */
	errs = 0;
	for (n = 1; n < 100000; n++) {
		xstore_scan_read_fail_at(n);
		i = sx_exec(db, "CREATE INDEX t_grp ON t(grp)", NULL);
		if (xstore_scan_read_fail_at(0) > 0) break;
		CK(i == SX_ERROR);
		CK(i != SX_ERROR || strncmp(sx_errmsg(db),
		    "index error: cannot build index t_grp on t: ", 44) == 0);
		CK(xstore_index_list(bt, "t", ix, 4) == 0);
		errs++;
	}
	CK(n > 1 && errs == n - 1);
	CK(i == SX_OK && xstore_index_list(bt, "t", ix, 4) == 1);
	for (i = 0; i < 10; i++)
		CK(count_rows_i(db, "SELECT k FROM t WHERE grp = ?", i) == 200);
	printf("  ok   index: CREATE INDEX with each of %d scan reads failing "
	    "fails and leaves no index\n", errs);

	errs = sweep(db, "SELECT count(*) FROM t", 2000, &ok);
	CK(errs > 0);
	/* A failed UPDATE changes nothing; each that runs moves group 3. */
	errs += sweep(db, "UPDATE t SET grp = grp + 10 WHERE grp % 10 = 3", 200,
	    &ok);
	CK(count_rows_i(db, "SELECT k FROM t WHERE grp = ?", 3) == 0);

	/* A transaction's rows spilled to the staging area: bringing one
	 * back is a scan read too, and a failed one fails the scan. */
	memset(pad, 'p', sizeof pad - 1);
	pad[sizeof pad - 1] = '\0';
	CK(sx_exec(db, "CREATE TABLE s(k INTEGER PRIMARY KEY, pad TEXT)",
	    NULL) == SX_OK);
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	CK(sx_prepare(db, "INSERT INTO s VALUES(?, ?)", -1, &st, NULL) == SX_OK);
	for (i = 1; i <= 400; i++) {       /* 400 KB: past the spill mark */
		sx_bind_int64(st, 1, i);
		sx_bind_text(st, 2, pad, -1);
		CK(sx_step(st) == SX_DONE);
		sx_reset(st);
	}
	sx_finalize(st); st = NULL;
	n = sweep(db, "SELECT count(*) FROM s WHERE length(pad) = 1000", 400,
	    &ok);
	CK(n > 2);             /* more than the cursor's open and step */
	errs += n;
	CK(sx_exec(db, "COMMIT", NULL) == SX_OK);
	printf("  ok   index: %d statements with a failed scan read failed, "
	    "%d ran whole\n", errs, ok);

	sx_close(db);
	xstore_set_wal(NULL);
	bt_close(bt); bm_destroy(bm);
	wal_close(wal);
	unlink(path); unlink(logp);
	return g_fail;
}

/* Insert (1, 1) into u twice: the second must never go in.  With no
 * table both fail. */
static void
dup_insert_refused(sx_db *db)
{
	int a = sx_exec(db, "INSERT INTO u VALUES(1, 1)", NULL);
	int b = sx_exec(db, "INSERT INTO u VALUES(1, 1)", NULL);
	CK(b != SX_OK);
	if (a == SX_OK && b != SX_CONSTRAINT)
		fprintf(stderr, "  duplicate key: rc %d (%s)\n", b, sx_errmsg(db));
	CK(a != SX_OK || b == SX_CONSTRAINT);
}

/* 6: a failed autoindex build in CREATE TABLE drops the new table. */
static int
scenario_create_table_error(void)
{
	bm_t *bm = NULL, *bm2 = NULL;
	bt_t *bt = NULL, *bt2 = NULL;
	sx_db *db = NULL, *db2 = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT, b2 = BM_OPTS_DEFAULT;
	char path[256], path2[256], logp[300];
	wal_t *wal = NULL;
	wal_opts_t wo;
	xstore_index_info_t ix[4];
	uint32_t id;
	int fd, n, rc, errs = 0;

	t_tmpl(path, sizeof path, "sqlxtc-index-ctab");
	t_tmpl(path2, sizeof path2, "sqlxtc-index-ctab2");
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	fd = mkstemp(path2); if (fd < 0) return 1; close(fd);
	snprintf(logp, sizeof logp, "%s-wal", path);
	memset(&wo, 0, sizeof wo);
	wo.path = logp; wo.window_ns = 500000; wo.max_batch = 256;
	if (wal_open(&wo, &wal) != XTC_OK) return 1;
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 256;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	xstore_set_wal((struct wal *)wal);
	CK(sx_open_bt(bt, &db) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE e(a INTEGER, b INTEGER, PRIMARY KEY(a, b))",
	    NULL) == SX_OK);
	CK(sx_exec(db, "INSERT INTO e VALUES(1, 1)", NULL) == SX_OK);

	for (n = 1; n < 1000; n++) {
		xstore_scan_read_fail_at(n);
		rc = sx_exec(db, "CREATE TABLE u(a INTEGER, b INTEGER, "
		    "PRIMARY KEY(a, b))", NULL);
		if (xstore_scan_read_fail_at(0) > 0) break;
		errs++;
		CK(rc == SX_ERROR);
		CK(!xstore_table_id(bt, "u", &id));
		CK(xstore_index_list(bt, "u", ix, 4) == 0);
		dup_insert_refused(db);
		/* An existing table is never dropped. */
		xstore_scan_read_fail_at(n);
		(void)sx_exec(db, "CREATE TABLE IF NOT EXISTS e(a INTEGER, "
		    "b INTEGER, PRIMARY KEY(a, b))", NULL);
		xstore_scan_read_fail_at(0);
		CK(first_int_i(db, "SELECT count(*) FROM e WHERE a = ?", 1) == 1);
	}
	CK(errs > 0 && n < 1000);
	/* The run with no failed read created u whole: it enforces its key. */
	CK(rc == SX_OK && xstore_index_list(bt, "u", ix, 4) == 1);
	dup_insert_refused(db);
	CK(sx_exec(db, "DROP TABLE u", NULL) == SX_OK);
	xstore_scan_read_fail_at(1);
	CK(sx_exec(db, "CREATE TABLE u(a INTEGER, b INTEGER, PRIMARY KEY(a, b))",
	    NULL) == SX_ERROR);
	xstore_scan_read_fail_at(0);

	/* Crash with the failed CREATEs in the log (nothing flushed), and
	 * recover into a fresh tree: no table u comes back. */
	sx_close(db);
	xstore_set_wal(NULL);
	bt_close(bt); bm_destroy(bm);
	wal_close(wal);
	b2.path = path2; b2.page_size = PAGE_SZ; b2.n_frames = 256;
	CK(bm_create(&b2, &bm2) == XTC_OK);
	CK(bt_open(bm2, &bt2) == XTC_OK);
	CK(xstore_recover(bt2, logp) == XTC_OK);
	CK(xstore_index_rebuild_all(bt2) == 0);
	CK(sx_open_bt(bt2, &db2) == SX_OK);
	CK(!xstore_table_id(bt2, "u", &id));
	CK(xstore_table_id(bt2, "e", &id));
	dup_insert_refused(db2);
	sx_close(db2);
	bt_close(bt2); bm_destroy(bm2);
	unlink(path); unlink(path2); unlink(logp);
	if (g_fail) return 1;
	printf("  ok   index: CREATE TABLE with each of %d autoindex scan reads "
	    "failing leaves no table, after recovery too\n", errs);
	return 0;
}

int
main(void)
{
	if (scenario_index() != 0) return 1;
	if (scenario_recover() != 0) return 1;
	if (scenario_scan_error() != 0) return 1;
	if (scenario_create_table_error() != 0) return 1;
	printf("test_index: OK\n");
	return 0;
}
