/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_insert_race.c
 *	Multi-row autocommit statements racing on plain threads keep their
 *	error class and their atomicity.
 *
 *	A multi-row autocommit statement runs as an implicit transaction
 *	(xstore_stmt_implicit_begin): its rows are buffered and committed
 *	together.  An INSERT whose key another session commits after the
 *	INSERT's own key check is then refused by the commit's write-write
 *	check, as a stale conflict.  The engine re-runs such a statement,
 *	and the re-run's key check reports the duplicate.  The client must
 *	see "UNIQUE constraint failed", as PostgreSQL reports it at every
 *	isolation level, never a serialization failure: before the re-run
 *	covered INSERT, 39 of 600 statements here failed with one.
 *
 *	Phase 1: NT threads each run PER statements "INSERT INTO p
 *	VALUES(k, id), (k + 1, id)" with k drawn from [0, NK).  Every
 *	failure must be UNIQUE, no pair may be half in, and the table must
 *	be exactly the union of the pairs that succeeded.
 *	Phase 2: NT threads each run PER "UPDATE t SET v = v + 1" over NR
 *	rows.  No partial statement and no lost update: every row equals
 *	the number of statements that succeeded.  A serialization failure
 *	is allowed here (a row lost to 8 writers in a row), a partial one
 *	never.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "xtc.h"
#include "t_tmp.h"

#define NT  4
#define PER 150
#define NK  400
#define NR  200

static int   g_fail;
static char  g_path[256];
static long  g_ok[NT], g_uniq[NT], g_other[NT];
static char  g_have[NK + 1];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

static int64_t
one(sx_db *h, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -2;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK) return -2;
	if (sx_step(st) == SX_ROW) v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

/* Phase 1 worker: random overlapping key pairs.  A success marks both
 * keys; a key marked twice means two sessions both inserted it. */
static void *
inserter(void *arg)
{
	long id = (long)arg;
	unsigned s = (unsigned)(id * 7919 + 13);
	sx_db *h = NULL;
	char q[128];
	int i, k;
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	for (i = 0; i < PER; i++) {
		s = s * 1103515245u + 12345u;
		k = (int)((s >> 8) % NK);
		snprintf(q, sizeof q, "INSERT INTO p VALUES(%d, %ld), (%d, %ld)",
		    k, id, k + 1, id);
		if (sx_exec(h, q, NULL) == SX_OK) {
			pthread_mutex_lock(&g_mu);
			CK(!g_have[k] && !g_have[k + 1]);
			g_have[k] = g_have[k + 1] = 1;
			pthread_mutex_unlock(&g_mu);
			g_ok[id]++;
		} else if (strncmp(sx_errmsg(h), "UNIQUE constraint failed",
		    24) == 0) {
			g_uniq[id]++;
		} else {
			fprintf(stderr, "  INSERT failed: %s\n", sx_errmsg(h));
			g_other[id]++;
		}
	}
	sx_close(h);
	return NULL;
}

/* Phase 2 worker: whole-table increments. */
static void *
updater(void *arg)
{
	long id = (long)arg;
	sx_db *h = NULL;
	int i;
	if (sx_open(g_path, &h) != SX_OK) { g_fail = 1; return NULL; }
	for (i = 0; i < PER; i++)
		if (sx_exec(h, "UPDATE t SET v = v + 1", NULL) == SX_OK)
			g_ok[id]++;
	sx_close(h);
	return NULL;
}

/* Run NT copies of fn and return the statements that succeeded. */
static long
run_threads(void *(*fn)(void *))
{
	pthread_t th[NT];
	long i, ok = 0;
	memset(g_ok, 0, sizeof g_ok);
	for (i = 0; i < NT; i++)
		(void)pthread_create(&th[i], NULL, fn, (void *)i);
	for (i = 0; i < NT; i++)
		(void)pthread_join(th[i], NULL);
	for (i = 0; i < NT; i++)
		ok += g_ok[i];
	return ok;
}

int
main(void)
{
	sx_db *h = NULL;
	char q[96], wal[300], dwb[300];
	long ok, uniq = 0, other = 0, want = 0, wrong = 0;
	int64_t n, sum, mn, mx;
	int fd, i;

	t_tmpl(g_path, sizeof g_path, "sqlxtc-insrace");
	if ((fd = mkstemp(g_path)) < 0) return 1;
	close(fd);
	unlink(g_path);
	snprintf(wal, sizeof wal, "%s-wal", g_path);
	snprintf(dwb, sizeof dwb, "%s.dwb", g_path);
	CK(sx_init() == SX_OK);
	CK(sx_storage_open(g_path, 4096) == SX_OK);
	CK(sx_open(g_path, &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER)",
	    NULL) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE p(k INTEGER PRIMARY KEY, w INTEGER)",
	    NULL) == SX_OK);
	CK(sx_exec(h, "BEGIN", NULL) == SX_OK);
	for (i = 0; i < NR; i++) {
		snprintf(q, sizeof q, "INSERT INTO t VALUES(%d, 0)", i);
		CK(sx_exec(h, q, NULL) == SX_OK);
	}
	CK(sx_exec(h, "COMMIT", NULL) == SX_OK);

	/* Phase 1. */
	ok = run_threads(inserter);
	for (i = 0; i < NT; i++) {
		uniq += g_uniq[i];
		other += g_other[i];
	}
	for (i = 0; i <= NK; i++) {
		want += g_have[i];
		snprintf(q, sizeof q, "SELECT count(*) FROM p WHERE k = %d", i);
		wrong += one(h, q) != g_have[i];
	}
	n = one(h, "SELECT count(*) FROM p");
	printf("  INSERT pairs: %ld ok, %ld UNIQUE, %ld other; rows %lld "
	    "want %ld, keys wrong %ld\n", ok, uniq, other, (long long)n, want,
	    wrong);
	CK(other == 0);
	CK(ok + uniq == NT * PER);
	CK(n == want);
	CK(wrong == 0);

	/* Phase 2. */
	ok = run_threads(updater);
	sum = one(h, "SELECT sum(v) FROM t");
	mn = one(h, "SELECT min(v) FROM t");
	mx = one(h, "SELECT max(v) FROM t");
	printf("  UPDATE all: %ld ok of %d; sum %lld want %lld; min %lld max "
	    "%lld\n", ok, NT * PER, (long long)sum, (long long)ok * NR,
	    (long long)mn, (long long)mx);
	CK(sum == ok * NR);
	CK(mn == ok && mx == ok);
	CK(ok > NT * PER / 2);

	sx_close(h);
	CK(sx_storage_close() == SX_OK);
	(void)sx_shutdown();
	unlink(g_path); unlink(wal); unlink(dwb);
	if (g_fail) {
		fprintf(stderr, "FAIL: test_insert_race\n");
		return 1;
	}
	printf("test_insert_race: OK\n");
	return 0;
}
