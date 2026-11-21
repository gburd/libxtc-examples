/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_point_read.c
 *	The engine's point read of a row by rowid at a snapshot (the fetch
 *	behind every secondary-index match and the max(rowid) liveness
 *	probe) is ONE first-key->=(table, rowid, snapshot) seek: no cursor,
 *	no walk over versions newer than the snapshot.
 *
 *	P1  an index seek returning 40 rows costs O(1) cursor descents
 *	    (the index walk itself), not one descent per fetched row.
 *	P4  an equality index probe of a single entry opens no cursor at
 *	    all: 200 of them on an index of unique values, 0 cursor
 *	    descents and 0 resumes.  Probes whose entries span leaves (the
 *	    40-row groups, and a 600-row group) return every row.
 *	P3  a rowid point SELECT (WHERE k = ?) is one seek too: no cursor
 *	    descent at all, and it returns the version of its snapshot, or
 *	    nothing for a deleted or absent rowid.
 *	P2  a row updated N times, read through its index at each of the
 *	    N snapshots, returns that snapshot's version; after an update
 *	    of the indexed column the old entry returns nothing at the
 *	    latest snapshot, the new one the row; a deleted row is gone.
 *	P5  a snapshot older than the newest row of the newest table reads
 *	    that row through an index and gets no row and no error, though
 *	    the key after the row's versions is an index entry longer than
 *	    a version key (the point read's key buffer must hold it).
 */

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
#include "t_tmp.h"

/* ThreadSanitizer builds compile the optimistic B-tree reads out
 * (btree.c BT_OLC_ENABLED), so a point read takes the latched fallback:
 * a cursor, one descent per read.  P1's bound holds only with OLC. */
#if defined(__SANITIZE_THREAD__)
#define T_OLC 0
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define T_OLC 0
#endif
#endif
#if !defined(T_OLC) && defined(BT_OLC_ENABLED)
#define T_OLC BT_OLC_ENABLED
#endif
#ifndef T_OLC
#define T_OLC 1
#endif

#define PAGE_SZ  4096
#define N_FRAMES 256
#define N_ROWS   4000
#define N_VER    6

/* The v column of the rows `sql` (one int parameter) returns, joined
 * with ','; the row count, or -1 on a prepare or step error. */
static int
rows_v(sx_db *db, const char *sql, int64_t p1, char *out, size_t cap)
{
	sx_stmt *st = NULL;
	size_t used = 0;
	int n = 0, rc;

	out[0] = '\0';
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK)
		return -1;
	sx_bind_int64(st, 1, p1);
	while ((rc = sx_step(st)) == SX_ROW) {
		const char *t = (const char *)sx_column_text(st, 0);
		int len = sx_column_bytes(st, 0);
		if (t != NULL && used + (size_t)len + 2 < cap) {
			if (n > 0) out[used++] = ',';
			memcpy(out + used, t, (size_t)len);
			used += (size_t)len;
			out[used] = '\0';
		}
		n++;
	}
	sx_finalize(st);
	return rc == SX_DONE ? n : -1;
}

int
main(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL;
	sx_stmt *st = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bt_stats_t s0, s1;
	char path[256], wal[288], sql[128], got[4096], want[32];
	int64_t ts[N_VER];
	uint64_t descents;
	int fd, i, n, ok;

	t_tmpl(path, sizeof path, "sqlxtc-pread");
	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES;
	bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &db) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, grp INTEGER, v TEXT)",
	    NULL) == SX_OK);
	CK(sx_prepare(db, "INSERT INTO t(k,grp,v) VALUES(?,?,?)", -1, &st, NULL)
	    == SX_OK);
	for (i = 1; i <= N_ROWS; i++) {
		char v[32];
		snprintf(v, sizeof v, "r%d", i);
		sx_bind_int64(st, 1, i);
		sx_bind_int64(st, 2, i % 100);       /* 100 groups of 40 */
		sx_bind_text(st, 3, v, -1);
		CK(sx_step(st) == SX_DONE);
		sx_reset(st);
	}
	sx_finalize(st);
	st = NULL;
	CK(sx_exec(db, "CREATE INDEX t_grp ON t(grp)", NULL) == SX_OK);
	/* A few versions on the rows of group 7, so a fetch at the latest
	 * snapshot has older versions behind the one it must return. */
	for (n = 0; n < 3; n++)
		for (i = 7; i <= N_ROWS; i += 100) {
			snprintf(sql, sizeof sql, "UPDATE t SET v='r%d' WHERE k=%d", i, i);
			CK(sx_exec(db, sql, NULL) == SX_OK);
		}

	/* P1: 40 index matches.  Before: one cursor descent per fetched
	 * row (41+).  Now: the index walk's own descent(s) only. */
	CK(rows_v(db, "SELECT v FROM t WHERE grp = ?", 7, got, sizeof got) == 40);
	bt_get_stats(bt, &s0);
	n = rows_v(db, "SELECT v FROM t WHERE grp = ?", 7, got, sizeof got);
	bt_get_stats(bt, &s1);
	descents = s1.descents - s0.descents;
	CK(n == 40);
	CK(strncmp(got, "r7,r107,r207,", 13) == 0);
	CK(!T_OLC || descents <= 4);
	printf("  %s   P1: index seek, %d rows fetched with %llu cursor "
	    "descents (one per row would be %d)%s\n",
	    !T_OLC || descents <= 4 ? "ok" : "FAIL", n,
	    (unsigned long long)descents, n + 1,
	    T_OLC ? "" : " [no OLC in this build: bound not checked]");

	/* P4: single-entry equality probes take no cursor. */
	CK(sx_exec(db, "CREATE INDEX t_v ON t(v)", NULL) == SX_OK);
	for (i = 0; i < 100; i++)
		if (rows_v(db, "SELECT v FROM t WHERE grp = ?", i, got,
		        sizeof got) != 40)
			n = -1;
	bt_get_stats(bt, &s0);
	for (i = 0; i < 200; i++) {
		char vq[64];
		snprintf(vq, sizeof vq, "SELECT k FROM t WHERE v = 'r%d'",
		    1 + (i * 37) % N_ROWS);
		if (rows_v(db, vq, 0, got, sizeof got) != 1)
			n = -1;
	}
	bt_get_stats(bt, &s1);
	descents = s1.descents - s0.descents;
	ok = n != -1 &&
	    (!T_OLC || (descents == 0 && s1.resumes == s0.resumes));
	CK(ok);
	printf("  %s   P4: 200 index equality probes, %llu cursor descents, "
	       "%llu resumes%s\n",
	    ok ? "ok" : "FAIL", (unsigned long long)descents,
	    (unsigned long long)(s1.resumes - s0.resumes),
	    T_OLC ? "" : " [no OLC in this build: bound not checked]");
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	for (i = 0; i < 600; i++) {
		snprintf(sql, sizeof sql,
		    "INSERT INTO t(k,grp,v) VALUES(%d,1001,'g')", 10000 + i);
		CK(sx_exec(db, sql, NULL) == SX_OK);
	}
	CK(sx_exec(db, "COMMIT", NULL) == SX_OK);
	n = rows_v(db, "SELECT v FROM t WHERE grp = ?", 1001, got, sizeof got);
	CK(n == 600);
	CK(sx_exec(db, "DELETE FROM t WHERE grp = 1001", NULL) == SX_OK);
	CK(sx_exec(db, "DROP INDEX t_v", NULL) == SX_OK);

	/* P3: rowid point reads.  Before: one cursor (descent) per read. */
	bt_get_stats(bt, &s0);
	for (i = 1; i <= 200; i++) {
		char want3[32];
		int64_t k = 1 + (i * 37) % N_ROWS;
		snprintf(want3, sizeof want3, "r%lld", (long long)k);
		if (rows_v(db, "SELECT v FROM t WHERE k = ?", k, got, sizeof got)
		    != 1 || strcmp(got, want3) != 0)
			n = -1;
	}
	bt_get_stats(bt, &s1);
	descents = s1.descents - s0.descents;
	CK(n != -1);
	CK(rows_v(db, "SELECT v FROM t WHERE k = ?", N_ROWS + 77, got, sizeof got)
	    == 0);
	CK(!T_OLC || descents == 0);
	printf("  %s   P3: 200 rowid point SELECTs, %llu cursor descents (one "
	    "per read would be 200)%s\n", (!T_OLC || descents == 0) && n != -1 ?
	    "ok" : "FAIL", (unsigned long long)descents,
	    T_OLC ? "" : " [no OLC in this build: bound not checked]");

	/* P2: versions.  Row 5000 in its own group 1000; version j has
	 * v = "vj", snapshot ts[j] taken after writing it. */
	CK(sx_exec(db, "INSERT INTO t(k,grp,v) VALUES(5000,1000,'v0')", NULL)
	    == SX_OK);
	ts[0] = xstore_clock_now();
	for (i = 1; i < N_VER; i++) {
		snprintf(sql, sizeof sql, "UPDATE t SET v='v%d' WHERE k=5000", i);
		CK(sx_exec(db, sql, NULL) == SX_OK);
		ts[i] = xstore_clock_now();
	}
	for (i = 0; i < N_VER; i++) {
		(void)xstore_as_of(db, ts[i]);
		snprintf(want, sizeof want, "v%d", i);
		CK(rows_v(db, "SELECT v FROM t WHERE grp = ?", 1000, got, sizeof got)
		    == 1 && strcmp(got, want) == 0);
		CK(rows_v(db, "SELECT v FROM t WHERE k = ?", 5000, got, sizeof got)
		    == 1 && strcmp(got, want) == 0);
	}
	(void)xstore_as_of(db, 0);
	/* Move the row to group 1001: the old index entry stays behind. */
	CK(sx_exec(db, "UPDATE t SET grp=1001, v='moved' WHERE k=5000", NULL)
	    == SX_OK);
	CK(rows_v(db, "SELECT v FROM t WHERE grp = ?", 1000, got, sizeof got) == 0);
	CK(rows_v(db, "SELECT v FROM t WHERE grp = ?", 1001, got, sizeof got)
	    == 1 && strcmp(got, "moved") == 0);
	/* (Reading the moved row through its OLD entry at an old snapshot
	 * is not checked: with no snapshot pinned the entry is reaped,
	 * xstore.h "time travel reaches only snapshots someone pins".) */
	CK(sx_exec(db, "DELETE FROM t WHERE k=5000", NULL) == SX_OK);
	CK(rows_v(db, "SELECT v FROM t WHERE grp = ?", 1001, got, sizeof got) == 0);
	CK(rows_v(db, "SELECT v FROM t WHERE k = ?", 5000, got, sizeof got) == 0);
	(void)xstore_as_of(db, ts[N_VER - 1]);    /* before the delete */
	CK(rows_v(db, "SELECT v FROM t WHERE k = ?", 5000, got, sizeof got) == 1 &&
	    strcmp(got, "v5") == 0);
	(void)xstore_as_of(db, 0);
	/* A point read inside a transaction sees its own buffered write. */
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	CK(sx_exec(db, "UPDATE t SET v='mine' WHERE k=7", NULL) == SX_OK);
	CK(rows_v(db, "SELECT v FROM t WHERE k = ?", 7, got, sizeof got) == 1 &&
	    strcmp(got, "mine") == 0);
	CK(sx_exec(db, "ROLLBACK", NULL) == SX_OK);
	CK(rows_v(db, "SELECT v FROM t WHERE k = ?", 7, got, sizeof got) == 1 &&
	    strcmp(got, "r7") == 0);
	if (!g_fail)
		printf("  ok   P2: a row with %d versions read through its index "
		    "and its rowid at each snapshot; a moved row found by its new "
		    "key only; a deleted row gone; a txn reads its own write\n",
		    N_VER);

	/* P5: the newest table, one tail row per round, read through an
	 * index at a snapshot taken just before the row's INSERT.  The
	 * table's version keys are the last ones, so the first key past the
	 * row is the first index entry; t_grp's entries are short, so drop
	 * it and let u_v's 40-byte entries come first. */
	CK(sx_exec(db, "DROP INDEX t_grp", NULL) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE u(k INTEGER PRIMARY KEY, v TEXT)", NULL)
	    == SX_OK);
	CK(sx_exec(db, "CREATE INDEX u_v ON u(v)", NULL) == SX_OK);
	ok = 1;
	for (i = 1; i <= 200; i++) {
		int64_t before = xstore_clock_now();
		snprintf(sql, sizeof sql,
		    "INSERT INTO u VALUES(%d, "
		    "'value-%06d-xxxxxxxxxxxxxxxx')",
		    i, i);
		CK(sx_exec(db, sql, NULL) == SX_OK);
		snprintf(sql, sizeof sql,
		    "SELECT v FROM u WHERE v = "
		    "'value-%06d-xxxxxxxxxxxxxxxx'",
		    i);
		(void)xstore_as_of(db, before);
		if (rows_v(db, sql, 0, got, sizeof got) != 0)
			ok = 0;
		(void)xstore_as_of(db, 0);
		if (rows_v(db, sql, 0, got, sizeof got) != 1)
			ok = 0;
	}
	CK(ok);
	printf("  %s   P5: 200 index reads of a newer tail row at an older "
	    "snapshot: no row, no error\n", ok ? "ok" : "FAIL");

	sx_close(db);
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);
	snprintf(wal, sizeof wal, "%s-wal", path);
	unlink(wal);
	if (g_fail) {
		fprintf(stderr, "test_point_read: FAILED\n");
		return 1;
	}
	printf("test_point_read: ALL PASS\n");
	return 0;
}
