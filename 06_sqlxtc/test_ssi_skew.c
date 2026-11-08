/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_ssi_skew.c
 *	Write skew under SERIALIZABLE with CONCURRENT commits.  Classic
 *	on-call invariant: rows x and y start at 1; each transaction reads
 *	both, and if x + y == 2 sets ITS OWN row to 0.  Run serially one of
 *	the two must see the other's write, so x + y >= 1 always holds.
 *	Pairs of transactions run in separate procs whose COMMITs overlap
 *	(each parks on the group-commit fsync), on 1 and 4 loops.
 *	Before the fix SSI validation ran before, and unserialized against,
 *	the concurrent commit: both validated before either was visible and
 *	both committed -> x + y == 0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btree.h"
#include "bufmgr.h"
#include "engine.h"
#include "wal.h"
#include "xstore.h"
#include "xtc.h"
#include "xtc_async.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define ROUNDS 200

static bt_t        *g_bt;
static wal_t       *g_wal;
static sx_db       *g_c[2];
static int g_bar, g_left;
static long g_both, g_violations, g_commits;

static int64_t
sum_xy(sx_db *db)
{
	sx_stmt *st = NULL;
	int64_t v = -1;
	if (sx_prepare(db, "SELECT sum(v) FROM t", -1, &st, NULL) != SX_OK)
		return -1;
	if (sx_step(st) == SX_ROW) v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

/* Two-party barrier: the k-th call by each worker returns once both
 * have made k calls (a monotonic counter, so it never needs a reset). */
static void
barrier(int *k)
{
	int target = 2 * ++(*k);
	xtc_atomic_fetch_add_i32(&g_bar, 1, xtc_mo_seq_cst);
	while (xtc_atomic_load_i32(&g_bar, xtc_mo_seq_cst) < target)
		(void)xtc_proc_sleep(20LL * 1000);
}

static void
worker(void *arg)
{
	long me = (long)arg;
	sx_db *db = g_c[me];
	char q[64];
	int r, ok, k = 0;
	for (r = 0; r < ROUNDS; r++) {
		barrier(&k);                     /* round start */
		CK(sx_exec(db, "BEGIN ISOLATION LEVEL SERIALIZABLE", NULL) == SX_OK);
		ok = (sum_xy(db) == 2);          /* on call if both still are */
		barrier(&k);                     /* both have read */
		if (ok) {
			snprintf(q, sizeof q, "UPDATE t SET v=0 WHERE k=%ld", me);
			CK(sx_exec(db, q, NULL) == SX_OK);
		}
		if (sx_exec(db, "COMMIT", NULL) == SX_OK && ok)
			xtc_atomic_fetch_add_long(&g_commits, 1,
			    xtc_mo_seq_cst);
		barrier(&k);                     /* both commits resolved */
		if (me == 0) {
			int64_t s = sum_xy(db);
			if (s < 1)
				xtc_atomic_fetch_add_long(&g_violations, 1,
				    xtc_mo_seq_cst);
			if (s == 0)
				xtc_atomic_fetch_add_long(&g_both, 1,
				    xtc_mo_seq_cst);
			CK(sx_exec(db, "UPDATE t SET v=1", NULL) == SX_OK);
		}
		barrier(&k);                     /* reset done */
	}
	if (xtc_atomic_fetch_sub_i32(&g_left, 1, xtc_mo_seq_cst) == 1)
		(void)wal_writer_stop(g_wal);
}

int
main(void)
{
	char logp[256], pg[256];
	wal_opts_t wo = { 0 };
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	xtc_exec_t *ex = NULL;
	xtc_proc_opts_t po = { .size = sizeof(xtc_proc_opts_t), .name = "skew" };
	xtc_pid_t pid;
	int fd, nloops;
	long i;

	/* SQL runs on fibers: use the server's 512 KiB stack (main.c), not
	 * the 64 KiB library default, which sanitizer frames can exceed. */
	if (xtc_set_stack_size(512u * 1024u) != XTC_OK) return 1;
	for (nloops = 1; nloops <= 4; nloops += 3) {
		t_tmpl(logp, sizeof logp, "sqlxtc-skew-log");
		t_tmpl(pg, sizeof pg, "sqlxtc-skew-pg");
		if ((fd = mkstemp(logp)) < 0) return 1;
		close(fd);
		if ((fd = mkstemp(pg)) < 0) return 1;
		close(fd);
		wo.path = logp;
		if (wal_open(&wo, &g_wal) != XTC_OK) return 1;
		bo.path = pg; bo.page_size = 4096; bo.n_frames = 256;
		if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &g_bt) != XTC_OK)
			return 1;
		xstore_set_wal((struct wal *)g_wal);
		for (i = 0; i < 2; i++)
			if (sx_open_bt(g_bt, &g_c[i]) != SX_OK) return 1;
		CK(sx_exec(g_c[0], "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER)", NULL) == SX_OK);
		CK(sx_exec(g_c[0], "INSERT INTO t(k,v) VALUES(0,1)", NULL) == SX_OK);
		CK(sx_exec(g_c[0], "INSERT INTO t(k,v) VALUES(1,1)", NULL) == SX_OK);
		xtc_atomic_store_i32(&g_bar, 0, xtc_mo_seq_cst);
		xtc_atomic_store_i32(&g_left, 2, xtc_mo_seq_cst);
		xtc_atomic_store_long(&g_both, 0, xtc_mo_seq_cst);
		xtc_atomic_store_long(&g_violations, 0, xtc_mo_seq_cst);
		xtc_atomic_store_long(&g_commits, 0, xtc_mo_seq_cst);
		if (xtc_exec_init(&ex, nloops) != XTC_OK) return 1;
		if (wal_writer_spawn(g_wal, xtc_exec_loop(ex, 0), NULL) != XTC_OK) return 1;
		for (i = 0; i < 2; i++)
			if (xtc_proc_spawn(xtc_exec_loop(ex, (int)(i % nloops)), worker,
			    (void *)i, &po, &pid) != XTC_OK)
				return 1;
		if (xtc_exec_run(ex) != XTC_OK) return 1;
		(void)xtc_exec_fini(ex);
		printf("  [%d loop%s] rounds=%d writer-commits=%ld write-skew "
		       "(x+y==0)=%ld\n",
		    nloops, nloops > 1 ? "s" : "", ROUNDS,
		    xtc_atomic_load_long(&g_commits, xtc_mo_seq_cst),
		    xtc_atomic_load_long(&g_both, xtc_mo_seq_cst));
		CK(xtc_atomic_load_long(&g_violations, xtc_mo_seq_cst) == 0);
		CK(xtc_atomic_load_long(&g_commits, xtc_mo_seq_cst) >=
		    ROUNDS); /* one of each pair commits */
		for (i = 0; i < 2; i++) sx_close(g_c[i]);
		xstore_set_wal(NULL);
		bt_close(g_bt);
		bm_destroy(bm);
		wal_close(g_wal);
		unlink(logp); unlink(pg);
	}
	if (g_fail) { fprintf(stderr, "FAIL: test_ssi_skew\n"); return 1; }
	printf("  ok   SERIALIZABLE: concurrent write skew is always refused\n");
	return 0;
}
