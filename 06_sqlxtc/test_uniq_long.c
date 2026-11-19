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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
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
