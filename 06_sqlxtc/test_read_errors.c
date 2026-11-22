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

int
main(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256];
	int fd;

	t_tmpl(path, sizeof path, "sqlxtc-read-errors");
	fd = mkstemp(path);
	if (fd < 0) return 1;
	close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 256;
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &bt) != XTC_OK ||
	    sx_open_bt(bt, &db) != SX_OK)
		return 1;
	case_pk(db, bt);
	sx_close(db);
	bt_close(bt); bm_destroy(bm);
	unlink(path);
	if (g_fail) return 1;
	printf("test_read_errors: OK\n");
	return 0;
}
