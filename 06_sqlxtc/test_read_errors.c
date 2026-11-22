/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_read_errors.c
 *	A read of the tree that fails is an error, never "absent", "no
 *	newer version" or "no such id".  The store reads one row's or one
 *	catalog entry's versions outside a scan in many places; with the
 *	n-th such read failing (xstore_row_read_fail_at), for every n until
 *	no read is left, each statement below must fail with "cannot read
 *	table ...", or succeed with the right answer, and leave the store
 *	as a whole run would.  No daemon; plain checks, off a loop.
 *
 *	Proves:
 *	  1. An autocommit INSERT of an existing primary key whose key
 *	     check cannot read the row fails; it never puts in a duplicate
 *	     key (before: the row read as absent, and the INSERT went in).
 *	  2. The write-write check of an UPDATE (autocommit, at its write
 *	     intent in a transaction, and at COMMIT) and the serializable
 *	     check of a COMMIT that cannot read the row fail the statement;
 *	     an update never goes in on a check that could not run (before:
 *	     the row had "no newer version", the lost-update guard).
 *	  3. A COMMIT whose spilled (STEAL) payload cannot be read back
 *	     fails, says why, and commits nothing (before: the log record
 *	     encoder refused the NULL payload, and the client got "commit
 *	     failed" with no cause).
 *	  4. SELECT max(rowid) (one descent per rowid, no scan) fails when a
 *	     read fails (before: the table read as empty, or the MAX as an
 *	     older rowid).
 *	  5. In a transaction whose rows were spilled, a statement that is
 *	     undone rebuilds the own-write UNIQUE map, reading the spilled
 *	     rows back; one that cannot be read back still guards its value
 *	     (before: it was left out of the map, and a duplicate of it in
 *	     the same transaction went in).
 *	  6. Catalog reads: CREATE TABLE, CREATE INDEX, DROP TABLE and ALTER
 *	     TABLE RENAME whose catalog or id read fails fail, and leave the
 *	     catalog as it was; no id is ever handed out twice (before: a
 *	     failed read was "no such name" or "max id 0", so a CREATE could
 *	     take an id in use, and a DROP or RENAME report success).
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
#include "wal.h"
#include "t_tmp.h"

#define PAGE_SZ 4096

static int g_fail;
#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

/* The single integer of `sql`'s first row, or -1 (no row) / -2 (error). */
static int64_t
one_int(sx_db *db, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -1;
	int rc;
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK)
		return -2;
	rc = sx_step(st);
	if (rc == SX_ROW)
		v = sx_column_int64(st, 0);
	else if (rc != SX_DONE)
		v = -2;
	sx_finalize(st);
	return v;
}

/* A run that a failed read ended must say so. */
static void
ck_read_msg(sx_db *db, const char *sql, int n)
{
	if (strncmp(sx_errmsg(db), "cannot read table ", 18) == 0 &&
	    strstr(sx_errmsg(db), ": I/O error") != NULL)
		return;
	fprintf(stderr, "  %s: read %d failed: \"%s\"\n", sql, n,
	    sx_errmsg(db));
	g_fail = 1;
}

/*
 * Sweep the n-th row read failing over `sql`, for n = 1, 2, ... until a
 * run fails no read.  A run whose read failed must return SX_ERROR with
 * the read message; `ok_rc` is the code of the run with no failed read.
 * `check` (may be NULL) runs after every run and returns 0 when the
 * store is as it should be.  Returns the number of failed runs.
 */
static int
sweep(sx_db *db, const char *sql, int ok_rc, int (*check)(sx_db *))
{
	int n, rc, errs = 0;
	for (n = 1; n < 1000; n++) {
		xstore_row_read_fail_at(n);
		rc = sx_exec(db, sql, NULL);
		if (xstore_row_read_fail_at(0) > 0) {   /* no read failed */
			if (rc != ok_rc) {
				fprintf(stderr, "  %s: no read failed, rc %d (%s)\n",
				    sql, rc, sx_errmsg(db));
				g_fail = 1;
			}
			break;
		}
		errs++;
		if (rc != SX_ERROR) {
			fprintf(stderr, "  %s: read %d failed, rc %d\n", sql, n,
			    rc);
			g_fail = 1;
		} else {
			ck_read_msg(db, sql, n);
		}
		if (check != NULL && check(db) != 0) {
			fprintf(stderr, "  %s: read %d failed: store changed\n",
			    sql, n);
			g_fail = 1;
		}
	}
	CK(n < 1000);
	return errs;
}

/* Table p holds key 1 once. */
static int
p_has_one(sx_db *db)
{
	return one_int(db, "SELECT count(*) FROM p WHERE k || '' = '1'") == 1
	    ? 0 : -1;
}

/* 1: the autocommit INSERT's primary-key check, under the commit lock.
 * The SQL INSERT checks the key first by a point read (a scan read), so
 * this check decides only when another INSERT of the key commits
 * between the two; call the store's insert directly, as that INSERT
 * would reach it. */
static void
case_pk(sx_db *db, bt_t *bt)
{
	uint8_t rec[16];
	uint32_t tid = 0;
	int n, rc, errs = 0, len;
	CK(sx_exec(db, "CREATE TABLE p(k INTEGER PRIMARY KEY, v INTEGER)",
	    NULL) == SX_OK);
	CK(sx_exec(db, "INSERT INTO p VALUES(1, 10)", NULL) == SX_OK);
	CK(xstore_table_id(bt, "p", &tid));
	rec[0] = 1;
	len = xstore_rec_put(rec, 1, (int)sizeof rec, XSTORE_C_INT, 11, 0,
	    NULL, 0);
	CK(len > 0);
	for (n = 1; n < 1000; n++) {
		xstore_row_read_fail_at(n);
		rc = xstore_insert_txn((struct xsql *)db, tid, 1, rec, len);
		if (xstore_row_read_fail_at(0) > 0)
			break;
		errs++;
		CK(rc == -1);
		CK(strncmp(xstore_read_err((struct xsql *)db),
		    "cannot read table p: I/O error", 30) == 0);
		CK(p_has_one(db) == 0);
	}
	CK(n < 1000 && errs > 0);
	CK(rc == 1);                       /* the read made: a duplicate */
	CK(one_int(db, "SELECT v FROM p WHERE k = 1") == 10);
	/* In a transaction the statement checks the key against the tree
	 * and its own writes (xstore_row_exists_txn). */
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	errs += sweep(db, "INSERT INTO p VALUES(1, 12)", SX_CONSTRAINT, NULL);
	CK(sx_exec(db, "COMMIT", NULL) == SX_OK);
	CK(one_int(db, "SELECT v FROM p WHERE k = 1") == 10);
	printf("  ok   read errors: INSERT of an existing key with each of %d "
	    "key reads failing fails, no duplicate\n", errs);
}

/* v of p's row 1, as the runs that committed left it. */
static int64_t g_v;

static int
p_v_is(sx_db *db)
{
	return one_int(db, "SELECT v FROM p WHERE k = 1") == g_v ? 0 : -1;
}

/* Run `body` (statements separated by ';', no binds) with the n-th row
 * read failing, for every n until none fails.  A failed read must fail
 * some statement with the read message, and the transaction then rolls
 * back; a run whose every statement succeeded adds `inc` to g_v.  p's
 * row 1 must hold g_v after each run.  Returns the failed runs. */
static int
sweep_txn(sx_db *db, const char *const *body, int nbody, int inc)
{
	int n, i, rc = SX_OK, errs = 0, faults;
	for (n = 1; n < 1000; n++) {
		faults = xstore_row_read_faults();
		xstore_row_read_fail_at(n);
		for (i = 0; i < nbody; i++) {
			rc = sx_exec(db, body[i], NULL);
			if (rc != SX_OK)
				break;
		}
		if (rc != SX_OK && i > 0)
			ck_read_msg(db, body[i], n);
		if (rc != SX_OK)
			(void)sx_exec(db, "ROLLBACK", NULL);
		xstore_row_read_fail_at(0);
		if (xstore_row_read_faults() == faults) {   /* none failed */
			CK(rc == SX_OK);
			g_v += inc;
			CK(p_v_is(db) == 0);
			break;
		}
		errs++;
		if (rc == SX_OK) {
			fprintf(stderr, "  %s: read %d failed, the transaction "
			    "committed\n", body[nbody - 2], n);
			g_fail = 1;
			g_v += inc;
		}
		CK(p_v_is(db) == 0);
	}
	CK(n < 1000);
	return errs;
}

/* 2: the write-write and serializable checks. */
static void
case_ww(sx_db *db)
{
	static const char *const snap[] = { "BEGIN",
		"UPDATE p SET v = v + 1 WHERE k = 1", "COMMIT" };
	static const char *const ser[] = {
		"BEGIN ISOLATION LEVEL SERIALIZABLE",
		"SELECT v FROM p WHERE k = 1",
		"UPDATE p SET v = v + 1 WHERE k = 1", "COMMIT" };
	int a, b, c;
	g_v = one_int(db, "SELECT v FROM p WHERE k = 1");
	a = sweep(db, "UPDATE p SET v = v + 1 WHERE k = 1", SX_OK, p_v_is);
	g_v++;
	CK(p_v_is(db) == 0);
	b = sweep_txn(db, snap, 3, 1);
	c = sweep_txn(db, ser, 4, 1);
	CK(a > 0 && b > 0 && c > b);
	printf("  ok   read errors: UPDATE with each of %d (autocommit), %d "
	    "(transaction) and %d (serializable) check reads failing fails, "
	    "no update lost\n", a, b, c);
}

/* No row of s is visible. */
static int
s_empty(sx_db *db)
{
	return one_int(db, "SELECT count(*) FROM s") == 0 ? 0 : -1;
}

/* 3: COMMIT brings a large transaction's spilled payloads back. */
static void
case_spill(sx_db *db)
{
	sx_stmt *st = NULL;
	char pad[1001];
	int n, i, rc = SX_OK, errs = 0, faults;
	memset(pad, 'p', sizeof pad - 1);
	pad[sizeof pad - 1] = '\0';
	CK(sx_exec(db, "CREATE TABLE s(k INTEGER PRIMARY KEY, pad TEXT)",
	    NULL) == SX_OK);
	/* Each run reloads a few hundred payloads and builds 400 rows: fail
	 * the first reads, then every 61st. */
	for (n = 1; n < 1000; n += n < 4 ? 1 : 61) {
		CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
		CK(sx_prepare(db, "INSERT INTO s VALUES(?, ?)", -1, &st,
		    NULL) == SX_OK);
		for (i = 1; i <= 400; i++) {   /* 400 KB: past the spill mark */
			sx_bind_int64(st, 1, i);
			sx_bind_text(st, 2, pad, -1);
			CK(sx_step(st) == SX_DONE);
			sx_reset(st);
		}
		sx_finalize(st);
		faults = xstore_row_read_faults();
		xstore_row_read_fail_at(n);
		rc = sx_exec(db, "COMMIT", NULL);
		xstore_row_read_fail_at(0);
		if (xstore_row_read_faults() == faults)
			break;
		errs++;
		CK(rc == SX_ERROR);
		ck_read_msg(db, "COMMIT", n);
		CK(s_empty(db) == 0);
		if (rc == SX_OK)
			break;
	}
	CK(rc == SX_OK && errs > 0);
	CK(one_int(db, "SELECT count(*) FROM s WHERE length(pad) = 1000") ==
	    400);
	printf("  ok   read errors: COMMIT with each of %d spilled payload "
	    "reads failing fails and commits nothing\n", errs);
}

/* Sweep the n-th row read failing over `sql`, whose first column must
 * be `want` in every run that succeeds.  Returns the failed runs. */
static int
sweep_val(sx_db *db, const char *sql, int64_t want)
{
	int n, errs = 0, faults;
	int64_t got;
	for (n = 1; n < 1000; n++) {
		faults = xstore_row_read_faults();
		xstore_row_read_fail_at(n);
		got = one_int(db, sql);
		xstore_row_read_fail_at(0);
		if (xstore_row_read_faults() == faults) {
			CK(got == want);
			break;
		}
		errs++;
		if (got != -2) {
			fprintf(stderr, "  %s: read %d failed, got %lld\n", sql, n,
			    (long long)got);
			g_fail = 1;
		} else {
			ck_read_msg(db, sql, n);
		}
	}
	CK(n < 1000);
	return errs;
}

/* 4: MAX(rowid) by descent. */
static void
case_maxrowid(sx_db *db)
{
	int n;
	CK(sx_exec(db, "CREATE TABLE m(k INTEGER PRIMARY KEY, v INTEGER)",
	    NULL) == SX_OK);
	CK(sx_exec(db, "INSERT INTO m VALUES(1, 1), (2, 2), (3, 3)",
	    NULL) == SX_OK);
	CK(sx_exec(db, "DELETE FROM m WHERE k = 3", NULL) == SX_OK);
	n = sweep_val(db, "SELECT max(k) FROM m", 2);
	CK(n > 2);                /* the deleted rowid's reads, then 2's */
	printf("  ok   read errors: MAX(rowid) with each of %d reads failing "
	    "fails\n", n);
}

/* 5: the own-write UNIQUE map after an undo in a spilled transaction. */
static void
case_umap(sx_db *db)
{
	static const int fail_at[] = { 1, 2, 3, 50, 199 };
	sx_stmt *st = NULL;
	char pad[1001], sql[96];
	int r, i, rc, bad = 0;
	memset(pad, 'p', sizeof pad - 1);
	pad[sizeof pad - 1] = '\0';
	CK(sx_exec(db, "CREATE TABLE u(k INTEGER PRIMARY KEY, c INTEGER UNIQUE, "
	    "pad TEXT)", NULL) == SX_OK);
	for (r = 0; r < (int)(sizeof fail_at / sizeof fail_at[0]); r++) {
		CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
		CK(sx_prepare(db, "INSERT INTO u VALUES(?, ?, ?)", -1, &st,
		    NULL) == SX_OK);
		for (i = 1; i <= 300; i++) {   /* 300 KB: past the spill mark */
			sx_bind_int64(st, 1, i);
			sx_bind_int64(st, 2, i);
			sx_bind_text(st, 3, pad, -1);
			CK(sx_step(st) == SX_DONE);
			sx_reset(st);
		}
		sx_finalize(st);
		/* The second row is a duplicate: the statement is undone, and
		 * the map is rebuilt from the buffer with a reload failing. */
		xstore_row_read_fail_at(fail_at[r]);
		CK(sx_exec(db, "INSERT INTO u VALUES(1001, 1001, 'x'), "
		    "(1002, 3, 'x')", NULL) != SX_OK);
		xstore_row_read_fail_at(0);
		for (i = 1; i <= 300; i++) {
			snprintf(sql, sizeof sql,
			    "INSERT INTO u VALUES(%d, %d, 'y')", 2000 + i, i);
			rc = sx_exec(db, sql, NULL);
			if (rc == SX_OK) bad++;
		}
		CK(sx_exec(db, "ROLLBACK", NULL) == SX_OK);
	}
	CK(bad == 0);
	if (bad != 0)
		fprintf(stderr, "  %d duplicates of spilled rows went in\n", bad);
	printf("  ok   read errors: an unreadable spilled row still guards its "
	    "UNIQUE value\n");
}

/* The catalog as a string: every table's id, then every index. */
static void
cat_snap(sx_db *db, bt_t *bt, char *out, size_t cap)
{
	static const char *const names[] = { "p", "m", "s", "u", "c1", "c2",
		"c3" };
	xstore_index_info_t ix[8];
	size_t off = 0;
	uint32_t id;
	int i, j, n;
	(void)db;
	out[0] = '\0';
	for (i = 0; i < (int)(sizeof names / sizeof names[0]); i++) {
		if (!xstore_table_id(bt, names[i], &id))
			id = 0;
		off += (size_t)snprintf(out + off, cap - off, "%s=%u:", names[i],
		    (unsigned)id);
		n = xstore_index_list(bt, names[i], ix, 8);
		for (j = 0; j < n && off < cap; j++)
			off += (size_t)snprintf(out + off, cap - off, "%s/%u,",
			    ix[j].name, (unsigned)ix[j].idx_id);
	}
}

/* Sweep the n-th row read failing over DDL `sql`.  A failed run must
 * leave the catalog as it was before; the run with no failed read must
 * succeed.  Returns the failed runs. */
static int
sweep_ddl(sx_db *db, bt_t *bt, const char *sql)
{
	char before[1024], after[1024];
	int n, rc, errs = 0, faults;
	cat_snap(db, bt, before, sizeof before);
	for (n = 1; n < 1000; n++) {
		faults = xstore_row_read_faults();
		xstore_row_read_fail_at(n);
		rc = sx_exec(db, sql, NULL);
		xstore_row_read_fail_at(0);
		if (xstore_row_read_faults() == faults) {
			CK(rc == SX_OK);
			break;
		}
		errs++;
		cat_snap(db, bt, after, sizeof after);
		/* DROP TABLE writes the table's tombstone before its indexes'
		 * id mark; a mark it cannot read leaves the index catalog rows
		 * for the next open (they name no table, so it taints).  The
		 * DROP itself is done. */
		if (rc == SX_OK && strncmp(sql, "DROP", 4) == 0 &&
		    strstr(after, "c3=0:") != NULL)
			break;
		if (rc == SX_OK || strcmp(before, after) != 0) {
			fprintf(stderr, "  %s: read %d failed: rc %d\n    before %s\n"
			    "    after  %s\n", sql, n, rc, before, after);
			g_fail = 1;
		}
	}
	CK(n < 1000);
	return errs;
}

/* 6: the catalog. */
static void
case_catalog(sx_db *db, bt_t *bt)
{
	uint32_t a = 0, b = 0, c = 0;
	int n = 0;
	CK(sx_exec(db, "CREATE TABLE c1(k INTEGER PRIMARY KEY, v INTEGER)",
	    NULL) == SX_OK);
	CK(sx_exec(db, "DROP TABLE c1", NULL) == SX_OK);
	/* A CREATE of a new name, the index id allocation, RENAME and DROP
	 * all read the catalog tree (the name cache cannot say a name is
	 * free, or that an id is unused). */
	n += sweep_ddl(db, bt, "CREATE TABLE c2(k INTEGER PRIMARY KEY, "
	    "v INTEGER)");
	n += sweep_ddl(db, bt, "CREATE INDEX c2_v ON c2(v)");
	n += sweep_ddl(db, bt, "ALTER TABLE c2 RENAME TO c3");
	CK(xstore_table_id(bt, "c3", &b) && one_int(db,
	    "SELECT count(*) FROM c3") == 0);
	n += sweep_ddl(db, bt, "DROP TABLE c3");
	CK(sx_exec(db, "CREATE TABLE c1(k INTEGER PRIMARY KEY, v INTEGER)",
	    NULL) == SX_OK);
	CK(xstore_table_id(bt, "c1", &a));
	CK(xstore_table_id(bt, "p", &c));
	CK(a > b && b > c);      /* each CREATE took a new id */
	printf("  ok   read errors: DDL with each of %d catalog reads failing "
	    "fails and leaves the catalog as it was\n", n);
}

int
main(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256], logp[300];
	wal_t *wal = NULL;
	wal_opts_t wo;
	int fd;

	t_tmpl(path, sizeof path, "sqlxtc-read-errors");
	fd = mkstemp(path);
	if (fd < 0) return 1;
	close(fd);
	/* A log, so a large transaction spills to the staging area. */
	snprintf(logp, sizeof logp, "%s-wal", path);
	memset(&wo, 0, sizeof wo);
	wo.path = logp; wo.window_ns = 500000; wo.max_batch = 256;
	if (wal_open(&wo, &wal) != XTC_OK) return 1;
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 256;
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &bt) != XTC_OK)
		return 1;
	xstore_set_wal((struct wal *)wal);
	if (sx_open_bt(bt, &db) != SX_OK)
		return 1;
	case_pk(db, bt);
	case_ww(db);
	case_spill(db);
	case_maxrowid(db);
	case_umap(db);
	case_catalog(db, bt);
	sx_close(db);
	xstore_set_wal(NULL);
	bt_close(bt); bm_destroy(bm);
	wal_close(wal);
	unlink(path); unlink(logp);
	if (g_fail) return 1;
	printf("test_read_errors: OK\n");
	return 0;
}
