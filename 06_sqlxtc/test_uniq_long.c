/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_uniq_long.c
 *	UNIQUE and index lookups over keys of every length, on 4 KiB pages
 *	(bt_max_key 497).
 *
 *	(1) A UNIQUE probe that fails is an error, never "no duplicate".
 *	    With the committed-index probe made to fail
 *	    (xstore_uniq_fail_inject), an autocommit INSERT, a statement
 *	    inside a transaction, and a COMMIT each fail with "index error:
 *	    cannot check UNIQUE t.s: I/O error", and no row is written.
 *	    Before the fix the probe's error read as "no duplicate" and every
 *	    write went through.
 *	(2) Long keys are indexed, found, and kept UNIQUE.  For TEXT values of
 *	    498 (bt_max_key + 1), 699, and 3000 bytes: a second row with the
 *	    same value is refused under UNIQUE (autocommit, and inside a
 *	    transaction against its own write); a row whose value differs only
 *	    after byte 497 is accepted; a lookup by the whole value through
 *	    the index finds exactly its row; and xstore_index_check reports no
 *	    entry missing.  A short duplicate is still refused.
 *	(3) A non-UNIQUE index finds a long-valued row by equality, and its
 *	    order (ORDER BY, MIN, MAX, a range) over values that share their
 *	    first 600 bytes is the values' order.
 *	(4) CREATE INDEX over existing long values: a plain index finds every
 *	    row; a UNIQUE index over two equal long values is refused with
 *	    "UNIQUE constraint failed: u.s", over two values equal only in
 *	    their first 600 bytes it is built and enforced.
 *	(5) The SipHash-1-3 the long-key encoding uses (sx_siphash.h) matches
 *	    Rust's SipHasher13 for the reference key and messages.
 *	(6) A B-tree read that fails inside an index scan or a UNIQUE probe
 *	    is an error, never a missing row.  For each n, the n-th read
 *	    fails (xstore_index_read_fail_at) until n passes the last read:
 *	    an equality, range, and reverse index scan (autocommit and over
 *	    the transaction's own writes, and over rows too wide for a leaf,
 *	    which only the point read fetches) each fail or return the right
 *	    count, and a duplicate long value is refused (a constraint error
 *	    or an index error) at every n, never inserted.  Before the fix a
 *	    failed candidate-row read made the probe say "no duplicate" and
 *	    a failed cursor read ended the scan early.
 *	(7) The same for a transaction whose buffered rows were spilled to
 *	    the STEAL staging area: the n-th read fails, and the first n of
 *	    them are the staging area's (bringing a spilled payload back).
 *	    An index scan over the transaction's own rows fails or counts
 *	    them all (also when a row is spilled again while the scan is
 *	    open), and a duplicate of a spilled row's UNIQUE value is
 *	    refused at every n.  Before the fix an unreadable spilled row
 *	    was skipped: the scan came up short and the duplicate went in.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "bufmgr.h"
#include "btree.h"
#include "sx_siphash.h"
#include "vexec.h"
#include "xstore.h"
#include "xtc.h"
#include "t_tmp.h"

#undef CK
#define CK(c)                                                                  \
	do {                                                                   \
		if (!(c)) {                                                    \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,          \
			    __LINE__, #c);                                     \
			g_fail = 1;                                            \
		}                                                              \
	} while (0)

#define PROBE_ERR "index error: cannot check UNIQUE t.s: I/O error"

static int64_t
one(sx_db *h, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -1;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK)
		return -2;
	if (sx_step(st) == SX_ROW)
		v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

/* INSERT (k, s) with s bound as text; returns the step's code. */
static int
ins(sx_db *h, int64_t k, const char *s)
{
	sx_stmt *st = NULL;
	int rc;
	if (sx_prepare(h, "INSERT INTO t(k, s) VALUES(?, ?)", -1, &st, NULL) !=
	    SX_OK)
		return -1;
	sx_bind_int64(st, 1, k);
	sx_bind_text(st, 2, s, -1);
	rc = sx_step(st);
	sx_finalize(st);
	return rc;
}

/* A value of n bytes: 'x' repeated, then a 8-digit tag in the last 8
 * bytes (n >= 8), so tagged values share their first n - 8 bytes. */
static char *
longval(int n, int tag)
{
	char *v = malloc((size_t)n + 1);
	if (v == NULL)
		abort();
	memset(v, 'x', (size_t)n);
	snprintf(v + n - 8, 9, "%08d", tag);
	return v;
}

/* Rows matching s = v (bound) through the planner, -1 on error. */
static int64_t
count_eq(sx_db *h, const char *sql, const char *v)
{
	sx_stmt *st = NULL;
	int64_t n = -1;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK)
		return -2;
	sx_bind_text(st, 1, v, -1);
	if (sx_step(st) == SX_ROW)
		n = sx_column_int64(st, 0);
	sx_finalize(st);
	return n;
}

/* The k of the one row with s = v, through the index: -1 if none or
 * more than one. */
static int64_t
k_of(sx_db *h, const char *v)
{
	sx_stmt *st = NULL;
	int64_t k = -1;
	int n = 0;
	if (sx_prepare(h, "SELECT k FROM t WHERE s = ?", -1, &st, NULL) !=
	    SX_OK)
		return -2;
	sx_bind_text(st, 1, v, -1);
	while (sx_step(st) == SX_ROW) {
		k = sx_column_int64(st, 0);
		n++;
	}
	sx_finalize(st);
	return n == 1 ? k : -1;
}

static int
no_missing(void)
{
	uint64_t miss = 1, stale = 0;
	(void)xstore_index_check(sx_storage_bt(), &miss, &stale);
	if (miss != 0)
		fprintf(stderr, "  xstore_index_check: %llu missing\n",
		    (unsigned long long)miss);
	return miss == 0;
}

static int
errmsg_is(sx_db *h, const char *want)
{
	const char *m = sx_errmsg(h);
	if (m != NULL && strcmp(m, want) == 0)
		return 1;
	fprintf(stderr, "  got error \"%s\", want \"%s\"\n", m ? m : "(null)",
	    want);
	return 0;
}

/* (1).  The COMMIT's probe runs only when something may have changed
 * since the statement check; an index created in between (on `b`)
 * guarantees it. */
static void
t_probe_error(sx_db *h, sx_db *b)
{
	xstore_uniq_fail_inject(1);
	CK(ins(h, 100, "p1") != SX_DONE);
	CK(errmsg_is(h, PROBE_ERR));
	xstore_uniq_fail_inject(0);
	CK(one(h, "SELECT count(*) FROM t WHERE k = 100") == 0);

	CK(sx_exec(h, "BEGIN", NULL) == SX_OK);
	xstore_uniq_fail_inject(1);
	CK(ins(h, 101, "p2") != SX_DONE);
	CK(errmsg_is(h, PROBE_ERR));
	xstore_uniq_fail_inject(0);
	CK(ins(h, 102, "p3") == SX_DONE);
	CK(sx_exec(b, "CREATE INDEX t_k ON t(k)", NULL) == SX_OK);
	xstore_uniq_fail_inject(1);
	CK(sx_exec(h, "COMMIT", NULL) != SX_OK);
	CK(errmsg_is(h, PROBE_ERR));
	xstore_uniq_fail_inject(0);
	CK(one(h, "SELECT count(*) FROM t WHERE k >= 100") == 0);
	if (!g_fail)
		printf(
		    "  ok   a failed UNIQUE probe fails the INSERT and the COMMIT\n");
}

/* (2) */
static void
t_long_unique(sx_db *h)
{
	static const int len[] = { 498, 699, 3000 };
	char *a, *b;
	int i;
	int64_t k;
	CK(bt_max_key(sx_storage_bt()) == 497);
	for (i = 0; i < 3; i++) {
		k = 1000 + 10 * i;
		a = longval(len[i], 1);
		b = longval(len[i], 2);
		CK(ins(h, k, a) == SX_DONE);
		CK(ins(h, k + 1, a) != SX_DONE);
		CK(errmsg_is(h, "UNIQUE constraint failed: t.s"));
		CK(ins(h, k + 2, b) == SX_DONE);
		CK(k_of(h, a) == k);
		CK(k_of(h, b) == k + 2);
		CK(count_eq(h, "SELECT count(*) FROM t WHERE s = ?", a) == 1);
		/* the same against the transaction's own buffered write */
		CK(sx_exec(h, "BEGIN", NULL) == SX_OK);
		free(a);
		a = longval(len[i], 3);
		CK(ins(h, k + 3, a) == SX_DONE);
		CK(ins(h, k + 4, a) != SX_DONE);
		CK(errmsg_is(h, "UNIQUE constraint failed: t.s"));
		CK(k_of(h, a) == k + 3);
		CK(sx_exec(h, "COMMIT", NULL) == SX_OK);
		CK(k_of(h, a) == k + 3);
		free(a);
		free(b);
	}
	CK(ins(h, 1100, "short") == SX_DONE);
	CK(ins(h, 1101, "short") != SX_DONE);
	CK(errmsg_is(h, "UNIQUE constraint failed: t.s"));
	CK(no_missing());
	if (!g_fail)
		printf(
		    "  ok   long UNIQUE keys: duplicates refused, near-equal "
		    "values kept, lookups exact\n");
}

/* (3) Values of 700 bytes sharing their first 600, inserted in an order
 * unrelated to the values. */
static void
t_long_order(sx_db *h)
{
	static const int tag[] = { 5, 3, 9, 1, 7, 2 };
	char *v;
	int i;
	int64_t prev, cur;
	uint64_t seeks, sorts;
	sx_stmt *st = NULL;
	CK(sx_exec(h, "CREATE TABLE o(k INTEGER PRIMARY KEY, s TEXT)", NULL) ==
	    SX_OK);
	CK(sx_exec(h, "CREATE INDEX o_s ON o(s)", NULL) == SX_OK);
	for (i = 0; i < 6; i++) {
		v = longval(700, tag[i]);
		memset(v + 600, 'a' + tag[i], 92); /* differ from byte 600 */
		CK(sx_prepare(h, "INSERT INTO o VALUES(?, ?)", -1, &st, NULL) ==
		    SX_OK);
		sx_bind_int64(st, 1, tag[i]);
		sx_bind_text(st, 2, v, -1);
		CK(sx_step(st) == SX_DONE);
		sx_finalize(st);
		CK(count_eq(h, "SELECT count(*) FROM o WHERE s = ?", v) == 1);
		free(v);
	}
	/* The walks below are index walks: seeks, and no sort. */
	seeks = xstore_index_seek_count();
	sorts = vx_sort_count();
	prev = -1;
	CK(sx_prepare(h, "SELECT k FROM o WHERE s > 'x' ORDER BY s", -1, &st,
	       NULL) == SX_OK);
	for (i = 0; sx_step(st) == SX_ROW; i++) {
		cur = sx_column_int64(st, 0);
		CK(cur > prev);
		prev = cur;
	}
	sx_finalize(st);
	CK(i == 6);
	prev = 100;
	CK(sx_prepare(h, "SELECT k FROM o ORDER BY s DESC", -1, &st, NULL) ==
	    SX_OK);
	for (i = 0; sx_step(st) == SX_ROW; i++) {
		cur = sx_column_int64(st, 0);
		CK(cur < prev);
		prev = cur;
	}
	sx_finalize(st);
	CK(i == 6);
	CK(one(h, "SELECT length(min(s)) FROM o") == 700);
	CK(one(h, "SELECT k FROM o WHERE s = (SELECT min(s) FROM o)") == 1);
	CK(one(h, "SELECT k FROM o WHERE s = (SELECT max(s) FROM o)") == 9);
	CK(xstore_index_seek_count() - seeks >= 4);
	CK(vx_sort_count() == sorts);
	CK(no_missing());
	if (!g_fail)
		printf("  ok   a plain index over long values: found, and in "
		       "value order\n");
}

/* Insert (k, s) into table `tab` (its first two columns). */
static int
ins_tab(sx_db *h, const char *tab, int64_t k, const char *s)
{
	sx_stmt *st = NULL;
	char sql[64];
	int rc;
	snprintf(sql, sizeof sql, "INSERT INTO %s(k, s) VALUES(?, ?)", tab);
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK)
		return -1;
	sx_bind_int64(st, 1, k);
	sx_bind_text(st, 2, s, -1);
	rc = sx_step(st);
	sx_finalize(st);
	return rc;
}

static int
ins_u(sx_db *h, int64_t k, const char *s)
{
	return ins_tab(h, "u", k, s);
}

/* (4) */
static void
t_create_over_long(sx_db *h)
{
	char *a = longval(800, 1), *b = longval(800, 2);
	CK(sx_exec(h, "CREATE TABLE u(k INTEGER PRIMARY KEY, s TEXT)", NULL) ==
	    SX_OK);
	CK(ins_u(h, 1, a) == SX_DONE);
	CK(ins_u(h, 2, a) == SX_DONE);
	CK(ins_u(h, 3, b) == SX_DONE);
	CK(sx_exec(h, "CREATE UNIQUE INDEX u_su ON u(s)", NULL) != SX_OK);
	CK(errmsg_is(h, "UNIQUE constraint failed: u.s"));
	CK(sx_exec(h, "CREATE INDEX u_s ON u(s)", NULL) == SX_OK);
	CK(count_eq(h, "SELECT count(*) FROM u WHERE s = ?", a) == 2);
	CK(count_eq(h, "SELECT count(*) FROM u WHERE s = ?", b) == 1);
	CK(sx_exec(h, "DELETE FROM u WHERE k = 2", NULL) == SX_OK);
	CK(sx_exec(h, "CREATE UNIQUE INDEX u_su ON u(s)", NULL) == SX_OK);
	CK(ins_u(h, 4, a) != SX_DONE);
	CK(errmsg_is(h, "UNIQUE constraint failed: u.s"));
	free(b);
	b = longval(800, 4);
	CK(ins_u(h, 5, b) == SX_DONE);
	CK(no_missing());
	free(a);
	free(b);
	if (!g_fail)
		printf("  ok   CREATE INDEX over existing long values\n");
}

/* (5) Reference: Rust's SipHasher13::new_with_keys(k0, k1), k = 00..0f,
 * message 00 01 .. (n - 1). */
static void
t_siphash(void)
{
	static const struct {
		int n;
		uint64_t h;
	} ref[] = {
		{ 0, 0xabac0158050fc4dcull },
		{ 1, 0xc9f49bf37d57ca93ull },
		{ 7, 0xd3927d989bb11140ull },
		{ 8, 0x369095118d299a8eull },
		{ 9, 0x25a48eb36c063de4ull },
		{ 15, 0xd320d86d2a519956ull },
		{ 16, 0xcc4fdd1a7d908b66ull },
		{ 63, 0x9d199062b7bbb3a8ull },
	};
	uint8_t m[64];
	int i;
	for (i = 0; i < 64; i++)
		m[i] = (uint8_t)i;
	for (i = 0; i < (int)(sizeof ref / sizeof ref[0]); i++)
		CK(sx_siphash13(0x0706050403020100ull, 0x0f0e0d0c0b0a0908ull, m,
		       (size_t)ref[i].n) == ref[i].h);
	if (!g_fail)
		printf("  ok   SipHash-1-3 matches the reference\n");
}

/* Run `sql` (one text bind `v`, NULL for none) with the n-th index read
 * failing, for n = 1, 2, .. until no read is left to fail.  Each run
 * must fail or count `want` rows.  Returns how many runs failed. */
static int
sweep_count(sx_db *h, const char *sql, const char *v, int64_t want)
{
	sx_stmt *st = NULL;
	int n, rc, errs = 0;
	int64_t got;
	for (n = 1; n < 1000; n++) {
		got = -1;
		CK(sx_prepare(h, sql, -1, &st, NULL) == SX_OK);
		if (v != NULL)
			sx_bind_text(st, 1, v, -1);
		xstore_index_read_fail_at(n);
		rc = sx_step(st);
		if (rc == SX_ROW)
			got = sx_column_int64(st, 0);
		sx_finalize(st);
		if (xstore_index_read_fail_at(0) > 0)
			break; /* fewer than n reads: nothing failed */
		if (rc == SX_ROW && got != want) {
			fprintf(stderr,
			    "  %s: read %d failed, count %lld, "
			    "want %lld or an error\n",
			    sql, n, (long long)got, (long long)want);
			g_fail = 1;
		}
		errs += rc != SX_ROW;
	}
	CK(n > 1 && n < 1000);
	return errs;
}

/* (6) */
static void
t_read_errors(sx_db *h)
{
	char *a = longval(699, 1), *b = longval(699, 2);
	char *wide = longval(2000, 3); /* over bt_max_inline on 4 KiB pages */
	int n, rc, errs;
	CK(sx_exec(h,
	       "CREATE TABLE r(k INTEGER PRIMARY KEY, s TEXT UNIQUE, "
	       "g INT)",
	       NULL) == SX_OK);
	CK(sx_exec(h, "CREATE INDEX r_g ON r(g)", NULL) == SX_OK);
	for (n = 1; n <= 300; n++) {
		sx_stmt *st = NULL;
		char sv[16];
		snprintf(sv, sizeof sv, "v%04d", n);
		CK(sx_prepare(h, "INSERT INTO r VALUES(?, ?, ?)", -1, &st,
		       NULL) == SX_OK);
		sx_bind_int64(st, 1, n);
		sx_bind_text(st, 2, n == 1 ? a : sv, -1);
		sx_bind_int64(st, 3, n % 3);
		CK(sx_step(st) == SX_DONE);
		sx_finalize(st);
	}
	errs = sweep_count(h, "SELECT count(*) FROM r WHERE s = ?", a, 1);
	errs += sweep_count(h, "SELECT count(*) FROM r WHERE g = 1", NULL, 100);
	errs += sweep_count(h, "SELECT count(*) FROM r WHERE g > 0 AND g < 2",
	    NULL, 100);
	errs += sweep_count(h,
	    "SELECT count(*) FROM (SELECT k FROM r "
	    "WHERE g = 1 ORDER BY g DESC LIMIT 1000)",
	    NULL, 100);
	CK(sx_exec(h, "BEGIN", NULL) == SX_OK);
	CK(sx_exec(h, "INSERT INTO r VALUES(1000, 'w', 1)", NULL) == SX_OK);
	errs += sweep_count(h, "SELECT count(*) FROM r WHERE g = 1", NULL, 101);
	CK(sx_exec(h, "ROLLBACK", NULL) == SX_OK);
	/* Rows too wide for a leaf: their values are out of line, which the
	 * batched row read never settles, so every row is read by the point
	 * read the batch falls back to (xs_point_get). */
	CK(sx_exec(h, "CREATE TABLE w(k INTEGER PRIMARY KEY, g INT, pad TEXT)",
	       NULL) == SX_OK);
	CK(sx_exec(h, "CREATE INDEX w_g ON w(g)", NULL) == SX_OK);
	for (n = 1; n <= 30; n++) {
		sx_stmt *st = NULL;
		CK(sx_prepare(h, "INSERT INTO w VALUES(?, ?, ?)", -1, &st,
		       NULL) == SX_OK);
		sx_bind_int64(st, 1, n);
		sx_bind_int64(st, 2, n % 3);
		sx_bind_text(st, 3, wide, -1);
		CK(sx_step(st) == SX_DONE);
		sx_finalize(st);
	}
	errs += sweep_count(h, "SELECT count(*) FROM w WHERE g = 1", NULL, 10);
	CK(errs > 0);
	/* A duplicate long value, then a new one: the duplicate never goes
	 * in, the new one goes in or fails. */
	for (n = 1; n < 1000; n++) {
		xstore_index_read_fail_at(n);
		rc = ins_tab(h, "r", 2000 + n, a);
		if (xstore_index_read_fail_at(0) > 0)
			break;
		CK(rc != SX_DONE);
	}
	CK(n > 1 && n < 1000);
	CK(ins_tab(h, "r", 2000, a) != SX_DONE);
	CK(errmsg_is(h, "UNIQUE constraint failed: r.s"));
	CK(one(h, "SELECT count(*) FROM r WHERE k >= 2000") == 0);
	CK(ins_tab(h, "r", 2001, b) == SX_DONE);
	CK(no_missing());
	free(a);
	free(b);
	free(wide);
	if (!g_fail)
		printf(
		    "  ok   a failed read in an index scan or a UNIQUE probe "
		    "is an error\n");
}

/* (7) */
static void
t_spilled_reads(sx_db *h)
{
	char *pad = longval(1000, 4), sv[16];
	const uint8_t *rec;
	xstore_iscan_t *sc;
	xstore_val_t v;
	uint32_t idx = 0;
	int64_t rid;
	int i, n, rc, rl, errs;
	CK(sx_exec(h,
	       "CREATE TABLE sp(k INTEGER PRIMARY KEY, s TEXT UNIQUE, g INT, "
	       "pad TEXT)",
	       NULL) == SX_OK);
	CK(sx_exec(h, "CREATE INDEX sp_g ON sp(g)", NULL) == SX_OK);
	/* 400 rows of 1 KB: past the 256 KB high-water mark, so the oldest
	 * buffered payloads (rows 1 to about 330) go to the staging area. */
	CK(sx_exec(h, "BEGIN", NULL) == SX_OK);
	for (n = 1; n <= 400; n++) {
		sx_stmt *st = NULL;
		snprintf(sv, sizeof sv, "s%04d", n);
		CK(sx_prepare(h, "INSERT INTO sp VALUES(?, ?, ?, ?)", -1, &st,
		       NULL) == SX_OK);
		sx_bind_int64(st, 1, n);
		sx_bind_text(st, 2, sv, -1);
		sx_bind_int64(st, 3, n % 4);
		sx_bind_text(st, 4, pad, -1);
		CK(sx_step(st) == SX_DONE);
		sx_finalize(st);
	}
	/* A read brings a spilled payload back for good, and the scan's
	 * open reads every buffered row of the table, so the probe goes
	 * first and reads row 2 (g = 2) alone; row 1 (g = 1) is then the
	 * first spilled row the scan reads. */
	for (n = 1; n < 1000; n++) {
		xstore_index_read_fail_at(n);
		rc = ins_tab(h, "sp", 1000 + n, "s0002");
		if (xstore_index_read_fail_at(0) > 0)
			break;
		CK(rc != SX_DONE);
	}
	CK(n > 1 && n < 1000);
	errs = sweep_count(h, "SELECT count(*) FROM sp WHERE g = 1", NULL, 100);
	CK(errs > 0);
	CK(one(h, "SELECT count(*) FROM sp WHERE k > 400 AND k < 500") == 0);
	/* A payload spilled again while a scan is open (a write between
	 * the open, which read every buffered row, and the rows' turn),
	 * with the n-th read of the walk failing. */
	CK(xstore_index_lookup(sx_storage_bt(), "sp", "g", &idx));
	memset(&v, 0, sizeof v);
	v.cls = XSTORE_C_INT;
	v.i = 1;
	for (i = 1; i < 1000; i++) {
		sc = xstore_iscan_open((struct xsql *)h, "sp", idx, &v, 1, NULL,
		    0, NULL, 0, 0);
		CK(sc != NULL);
		snprintf(sv, sizeof sv, "t%04d", i);
		CK(ins_tab(h, "sp", 500 + i, sv) == SX_DONE);
		xstore_index_read_fail_at(i);
		n = 0;
		while ((rc = xstore_iscan_next(sc, &rid, &rec, &rl)) == 1)
			n++;
		xstore_iscan_close(sc);
		if (xstore_index_read_fail_at(0) > 0)
			break;
		CK(rc < 0 || n == 100);
	}
	CK(i > 1 && i < 1000);
	CK(sx_exec(h, "ROLLBACK", NULL) == SX_OK);
	free(pad);
	if (!g_fail)
		printf("  ok   a spilled row that cannot be read back is an "
		       "error\n");
}

int
main(void)
{
	char path[256], side[320];
	const char *sfx[] = { "", "-wal", ".dwb" };
	sx_db *h = NULL, *b = NULL;
	int fd, i;

	t_tmpl(path, sizeof path, "sqlxtc-uniqlong");
	if ((fd = mkstemp(path)) < 0)
		return 1;
	close(fd);
	unlink(path);
	if (sx_init() != SX_OK || sx_storage_open(path, 1024) != SX_OK ||
	    sx_open(path, &h) != SX_OK || sx_open(path, &b) != SX_OK)
		return 1;
	CK(sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, s TEXT UNIQUE)",
	       NULL) == SX_OK);
	t_probe_error(h, b);
	t_long_unique(h);
	t_long_order(h);
	t_create_over_long(h);
	t_siphash();
	t_read_errors(h);
	t_spilled_reads(h);
	sx_close(b);
	sx_close(h);
	(void)sx_storage_close();
	(void)sx_shutdown();
	for (i = 0; i < 3; i++) {
		snprintf(side, sizeof side, "%s%s", path, sfx[i]);
		unlink(side);
	}
	if (g_fail) {
		fprintf(stderr, "FAIL: test_uniq_long\n");
		return 1;
	}
	printf("  ok   test_uniq_long\n");
	return 0;
}
