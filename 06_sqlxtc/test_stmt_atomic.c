/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_stmt_atomic.c
 *	An SQL statement is atomic: an autocommit INSERT ... SELECT, UPDATE
 *	or DELETE that changes several rows and fails partway leaves none of
 *	its rows committed, and one that succeeds leaves all of them.
 *	Before, each row of such a statement was its own commit, so a
 *	failure partway left the earlier rows committed while the client was
 *	told the statement failed; a client that retried applied it twice.
 *
 *	Each statement runs on fresh tables, once per failure, and the
 *	tables are compared with their state before it.  The failures:
 *	  (a) a read error: the n-th base-scan read, and separately the n-th
 *	      row read, fails (xstore_scan_read_fail_at,
 *	      xstore_row_read_fail_at), for every n until no read is left;
 *	  (b) a UNIQUE or PRIMARY KEY violation met by a later row;
 *	  (c) a row too large to store met by a later row (sqlxtc enforces
 *	      no NOT NULL or type constraint; this is its row-level error);
 *	  (d) a kill of the connection's proc that lands while the statement
 *	      runs (the kill is deferred to the statement's end, see
 *	      sx_step): the connection never hears an answer, and the
 *	      statement's rows must not be committed by a dead connection.
 *	A line per statement reports the failed runs and how many of them
 *	left rows committed; the test fails if any did.
 *
 *	usage: test_stmt_atomic
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
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_runtime.h"
#include "t_tmp.h"

#define PAGE_SZ 4096
#define NKILL   400      /* rows the killed statement changes */

static int g_fail;
#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

/* Every row of the tables a case uses, as (table, key, value) with the
 * value folding in p's length for s: what a statement may change. */
#define MAXR (NKILL + 64)
struct snap {
	int     n;
	int64_t t[3 * MAXR], k[3 * MAXR], v[3 * MAXR];
};

static void
snap_table(sx_db *db, int t, const char *sql, struct snap *o)
{
	sx_stmt *st = NULL;
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK) {
		g_fail = 1;
		return;
	}
	while (sx_step(st) == SX_ROW && o->n < 3 * MAXR) {
		o->t[o->n] = t;
		o->k[o->n] = sx_column_int64(st, 0);
		o->v[o->n] = sx_column_int64(st, 1);
		o->n++;
	}
	sx_finalize(st);
}

static void
state(sx_db *db, struct snap *o)
{
	o->n = 0;
	snap_table(db, 0, "SELECT k, v * 100000 + length(p) FROM s", o);
	snap_table(db, 1, "SELECT k, v FROM d", o);
	snap_table(db, 2, "SELECT k, c FROM u", o);
}

/* The rows that differ between a and b: in one and not the other, or
 * with another value.  Tables are small: quadratic is fine. */
static int
rows_changed(const struct snap *a, const struct snap *b)
{
	int i, j, n = 0, hit;
	for (i = 0; i < a->n; i++) {
		hit = 0;
		for (j = 0; j < b->n && !hit; j++)
			hit = a->t[i] == b->t[j] && a->k[i] == b->k[j];
		j--;
		if (!hit || a->v[i] != b->v[j])
			n++;
	}
	for (j = 0; j < b->n; j++) {
		hit = 0;
		for (i = 0; i < a->n && !hit; i++)
			hit = a->t[i] == b->t[j] && a->k[i] == b->k[j];
		n += !hit;
	}
	return n;
}

/* Fresh tables: s(k, v, p) holds k = 1..5 with v = k and a short p; d
 * is empty but for key 9; u(k, c UNIQUE) holds (1,5), (2,7), (3,9),
 * (4,14), so c * 2 collides with row 4 at row 2. */
static void
fresh(sx_db *db)
{
	(void)sx_exec(db, "DROP TABLE s", NULL);
	(void)sx_exec(db, "DROP TABLE d", NULL);
	(void)sx_exec(db, "DROP TABLE u", NULL);
	CK(sx_exec(db, "CREATE TABLE s(k INTEGER PRIMARY KEY, v INTEGER, "
	    "p TEXT)", NULL) == SX_OK);
	CK(sx_exec(db, "INSERT INTO s VALUES(1, 1, 'a'), (2, 2, 'b'), "
	    "(3, 3, 'c'), (4, 4, 'd'), (5, 5, 'e')", NULL) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE d(k INTEGER PRIMARY KEY, v INTEGER "
	    "UNIQUE)", NULL) == SX_OK);
	CK(sx_exec(db, "INSERT INTO d VALUES(9, 30)", NULL) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE u(k INTEGER PRIMARY KEY, c INTEGER "
	    "UNIQUE)", NULL) == SX_OK);
	CK(sx_exec(db, "INSERT INTO u VALUES(1, 5), (2, 7), (3, 9), (4, 14)",
	    NULL) == SX_OK);
}

static struct snap g_before, g_after;

/* Run `sql` with `arm` armed to fail read n (NULL: no fault) on the
 * tables as they are.  Returns the statement's rc; *left is the rows a
 * failed run changed (0 for a run that succeeded), *nofault 1 if no read
 * failed. */
static int
run_kept(sx_db *db, const char *sql, int (*arm)(int), int n, int *left,
    int *nofault)
{
	int rc;
	state(db, &g_before);
	if (arm != NULL)
		(void)arm(n);
	rc = sx_exec(db, sql, NULL);
	*nofault = arm == NULL || arm(0) > 0;
	state(db, &g_after);
	*left = rc != SX_OK ? rows_changed(&g_before, &g_after) : 0;
	return rc;
}

/* run_kept on fresh tables. */
static int
run_one(sx_db *db, const char *sql, int (*arm)(int), int n, int *left,
    int *nofault)
{
	fresh(db);
	return run_kept(db, sql, arm, n, left, nofault);
}

/* One line of the scope table: the failed runs, those that left rows
 * committed, and the most rows one of them left. */
static void
report(const char *cls, const char *sql, int fails, int bad, int most)
{
	printf("  scope %-4s %-46.46s failed %3d, left rows in %3d "
	    "(most %d)\n", cls, sql, fails, bad, most);
	if (bad != 0)
		g_fail = 1;
}

/* (a): sweep both read hooks over `sql`. */
static void
case_read(sx_db *db, const char *sql)
{
	int (*arms[2])(int) = { xstore_scan_read_fail_at,
	    xstore_row_read_fail_at };
	int a, n, rc, ch, nf, fails = 0, bad = 0, most = 0;
	for (a = 0; a < 2; a++)
		for (n = 1; n < 400; n++) {
			rc = run_one(db, sql, arms[a], n, &ch, &nf);
			if (nf) {
				CK(rc == SX_OK);
				break;
			}
			fails += rc != SX_OK;
			bad += ch > 0;
			if (ch > most) most = ch;
		}
	report("(a)", sql, fails, bad, most);
}

/* (b), (c): a statement that must fail on a later row; `kept`: on the
 * tables as the caller left them. */
static void
case_fail(sx_db *db, const char *cls, const char *sql, int kept)
{
	int rc, ch, nf;
	rc = kept ? run_kept(db, sql, NULL, 0, &ch, &nf) :
	    run_one(db, sql, NULL, 0, &ch, &nf);
	CK(rc != SX_OK);
	report(cls, sql, rc != SX_OK, ch > 0, ch);
}

/* (d): the kill.  The proc masks, is killed while masked (so the kill
 * is pending all through the statement), runs the statement, and dies
 * at the mask's end; the boss then counts what it left. */
static struct {
	sx_db      *db;
	wal_t      *wal;
	const char *sql;
	xtc_loop_t *loop;
	int         ready, ran, rc;
} g_k;

static void
k_conn(void *arg)
{
	(void)arg;
	(void)xtc_mask_enter();
	xtc_atomic_store_i32(&g_k.ready, 1, xtc_mo_seq_cst);
	(void)xtc_proc_sleep(50LL * 1000 * 1000);     /* killed in here */
	g_k.rc = sx_exec(g_k.db, g_k.sql, NULL);
	xtc_atomic_store_i32(&g_k.ran, 1, xtc_mo_seq_cst);
	(void)xtc_mask_leave();                       /* dies here */
	xtc_atomic_store_i32(&g_k.ran, 2, xtc_mo_seq_cst);   /* not killed */
}

static void
k_boss(void *arg)
{
	xtc_proc_opts_t po = { .size = sizeof(xtc_proc_opts_t), .name = "k" };
	xtc_pid_t pid;
	void *m;
	size_t mn;
	uint64_t ref;
	int i;
	(void)arg;
	if (xtc_proc_spawn_monitor(g_k.loop, k_conn, NULL, &po, &pid,
	    &ref) != XTC_OK) {
		g_fail = 1;
		goto out;
	}
	while (!xtc_atomic_load_i32(&g_k.ready, xtc_mo_seq_cst))
		(void)xtc_proc_sleep(1LL * 1000 * 1000);
	CK(xtc_exit_pid(pid, 7) == XTC_OK);
	for (i = 0; i < 20000; i++) {
		if (xtc_recv(&m, &mn, 0) == XTC_OK) {
			xtc_free(m);
			break;
		}
		(void)xtc_proc_sleep(1LL * 1000 * 1000);
	}
out:
	(void)wal_writer_stop(g_k.wal);
}

/* The killed statement on a log-backed store, so each commit parks on
 * the log as a server's does.  Returns the rows the dead statement
 * left committed. */
static int
kill_run(const char *sql)
{
	char path[256], logp[300];
	wal_opts_t wo;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	xtc_exec_t *ex = NULL;
	xtc_proc_opts_t po = { .size = sizeof(xtc_proc_opts_t), .name = "b" };
	xtc_pid_t pid;
	char ins[96];
	int fd, i, ch;
	t_tmpl(path, sizeof path, "sqlxtc-stmt-atomic-k");
	if ((fd = mkstemp(path)) < 0) return -1;
	close(fd);
	snprintf(logp, sizeof logp, "%s-wal", path);
	memset(&wo, 0, sizeof wo);
	wo.path = logp; wo.window_ns = 200000; wo.max_batch = 256;
	memset(&g_k, 0, sizeof g_k);
	g_k.sql = sql;
	if (wal_open(&wo, &g_k.wal) != XTC_OK) return -1;
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 512;
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &bt) != XTC_OK)
		return -1;
	xstore_set_wal((struct wal *)g_k.wal);
	if (sx_open_bt(bt, &g_k.db) != SX_OK) return -1;
	fresh(g_k.db);
	CK(sx_exec(g_k.db, "BEGIN", NULL) == SX_OK);
	for (i = 10; i < 10 + NKILL; i++) {
		snprintf(ins, sizeof ins, "INSERT INTO s VALUES(%d, %d, 'x')",
		    i, i);
		CK(sx_exec(g_k.db, ins, NULL) == SX_OK);
	}
	CK(sx_exec(g_k.db, "COMMIT", NULL) == SX_OK);
	state(g_k.db, &g_before);
	if (xtc_exec_create(&(xtc_exec_opts_t){ .size = sizeof(xtc_exec_opts_t),
	    .n_loops = 1 }, &ex) != XTC_OK)
		return -1;
	g_k.loop = xtc_exec_loop(ex, 0);
	if (wal_writer_spawn(g_k.wal, xtc_exec_loop(ex, 0), NULL) != XTC_OK ||
	    xtc_proc_spawn(xtc_exec_loop(ex, 0), k_boss, NULL, &po, &pid) !=
	    XTC_OK || xtc_exec_run(ex) != XTC_OK)
		return -1;
	xtc_exec_destroy(ex);
	CK(g_k.ran == 1);                  /* ran, then died at its end */
	state(g_k.db, &g_after);
	ch = rows_changed(&g_before, &g_after);
	sx_close(g_k.db);
	xstore_set_wal(NULL);
	bt_close(bt); bm_destroy(bm);
	wal_close(g_k.wal);
	unlink(path); unlink(logp);
	return ch;
}

int
main(void)
{
	static const char *const reads[] = {
		"INSERT INTO d SELECT k, v FROM s",
		"UPDATE s SET v = v + 1",
		"DELETE FROM s WHERE v > 0",
	};
	/* Over s's 405 rows (d's key 9 and value 30 are out of the way). */
	static const char *const kills[] = {
		"INSERT INTO d SELECT k + 1000, v + 100000 FROM s",
		"UPDATE s SET v = v + 1",
		"DELETE FROM s WHERE v > 0",
	};
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256], big[2600], sql[2700];
	size_t i;
	int r;

	if (xtc_runtime_set_stack_size(512u * 1024u) != XTC_OK) return 1;
	t_tmpl(path, sizeof path, "sqlxtc-stmt-atomic");
	if ((r = mkstemp(path)) < 0) return 1;
	close(r);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 256;
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &bt) != XTC_OK ||
	    sx_open_bt(bt, &db) != SX_OK)
		return 1;

	for (i = 0; i < sizeof reads / sizeof reads[0]; i++)
		case_read(db, reads[i]);
	/* (b): row 3's v * 10 = 30 is d's UNIQUE value; u's c * 2 at row 2
	 * is row 4's c; s's row 3 is a key d's INSERT VALUES repeats. */
	case_fail(db, "(b)", "INSERT INTO d SELECT k, v * 10 FROM s", 0);
	case_fail(db, "(b)", "UPDATE u SET c = c * 2", 0);
	case_fail(db, "(b)", "INSERT INTO s SELECT k + 2, v FROM s", 0);
	/* (c): row 3's p, 2500 bytes, doubles past the 4096-byte row;
	 * rows 1 and 2 come first and fit. */
	memset(big, 'x', 2500);
	big[2500] = '\0';
	snprintf(sql, sizeof sql, "UPDATE s SET p = '%s' WHERE k = 3", big);
	fresh(db);
	CK(sx_exec(db, sql, NULL) == SX_OK);
	case_fail(db, "(c)", "UPDATE s SET p = p || p", 1);
	/* (d) */
	for (i = 0; i < sizeof kills / sizeof kills[0]; i++) {
		r = kill_run(kills[i]);
		report("(d)", kills[i], 1, r != 0, r);
	}

	sx_close(db);
	bt_close(bt); bm_destroy(bm);
	unlink(path);
	if (g_fail) {
		fprintf(stderr, "FAIL: test_stmt_atomic\n");
		return 1;
	}
	printf("test_stmt_atomic: OK\n");
	return 0;
}
