/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_seg.c
 *	The segmented WAL (wal_opts_t.segment_bytes, STORAGE_DESIGN 6.2):
 *	  group      committers on one loop and on a 4-loop executor fill
 *	             many small segments; every commit's WAL address names
 *	             its own body (read back from the segment file), LSNs
 *	             are 1..N, and a scan of the segments sees exactly the
 *	             committed records in LSN order with those addresses;
 *	  frames     no frame spans two segments, and a frame larger than
 *	             a segment (direct path) gets one of its own;
 *	  release    wal_set_release lets a roll unlink only whole segments
 *	             below the boundary, oldest first; a scan from a deleted
 *	             segment is XTC_E_NOTFOUND, from the boundary it works;
 *	  reopen     append resumes LSNs and positions past the last
 *	             complete frame, a torn tail of the newest segment is
 *	             dropped, and a newest segment with no header (a roll
 *	             that crashed after its create) is removed;
 *	  lsn        the LSN never restarts: not after a release at the log
 *	             end plus a roll, a reopen and another roll (no segment
 *	             holds a frame), and not after a crash between a roll's
 *	             unlink and the new segment's first frame;
 *	  chain      a damaged frame in an older segment, or a missing
 *	             middle segment, refuses both the append open and the
 *	             scan (XTC_E_IO): no commit is acknowledged past a point
 *	             recovery cannot read;
 *	  refuse     wal_checkpoint, the online checkpoint and wal_truncate
 *	             refuse a segmented log; a one-file log reports wa 0.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wal.h"
#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define SEG_BYTES     (64u * 1024u)
#define N_COMMITTERS  8
#define PER_COMMITTER 200
#define N_COMMITS     (N_COMMITTERS * PER_COMMITTER)
#define FIRST_OFF     (WAL_SEG_HDR + 12u)  /* the first frame's body */

static char g_dir[256];
static wal_t *g_wal;
static int g_left;
static uint64_t g_wa[N_COMMITS + 1];     /* by LSN */
static uint32_t g_len[N_COMMITS + 1];
static int g_fail_commit;

/* Record bytes: the committer, the index, and filler so commits differ
 * in size (some near a quarter of a segment). */
static uint32_t
make_rec(char *buf, size_t cap, long id, int i)
{
	int n = snprintf(buf, cap, "rec-%02ld-%04d:", id, i);
	uint32_t fill = (uint32_t)((id * 131 + i * 17) % 3000);

	if ((size_t)n + fill > cap)
		fill = (uint32_t)(cap - (size_t)n);
	memset(buf + n, 'a' + (int)(id % 26), fill);
	return (uint32_t)n + fill;
}

static void
committer_proc(void *arg)
{
	long id = (long)arg;
	char rec[4096];
	int i;

	for (i = 0; i < PER_COMMITTER; i++) {
		uint64_t lsn = 0, wa = 0;
		uint32_t n = make_rec(rec, sizeof rec, id, i);
		if (wal_commit_wa(g_wal, rec, n, &lsn, &wa) != XTC_OK ||
		    lsn == 0 || lsn > N_COMMITS) {
			xtc_atomic_store_i32(&g_fail_commit, 1, xtc_mo_relaxed);
			continue;
		}
		g_wa[lsn] = wa;
		g_len[lsn] = n;
	}
	if (xtc_atomic_fetch_sub_i32(&g_left, 1, xtc_mo_seq_cst) == 1)
		(void)wal_writer_stop(g_wal);
}

static void
seg_path(uint32_t seg, char *out, size_t cap)
{
	snprintf(out, cap, "%s/wal-%08u.log", g_dir, seg);
}

/* Read `len` body bytes at WAL address wa straight from the segment. */
static int
read_wa(uint64_t wa, void *buf, uint32_t len)
{
	char p[300];
	int fd, ok;

	seg_path(WAL_WA_SEG(wa), p, sizeof p);
	if ((fd = open(p, O_RDONLY)) < 0)
		return 0;
	ok = pread(fd, buf, len, (off_t)WAL_WA_OFF(wa)) == (ssize_t)len;
	close(fd);
	return ok;
}

struct scan_acc {
	uint64_t expect, last_wa;
	int count, bad;
};

static int
scan_cb(uint64_t lsn, uint64_t wa, const void *rec, uint32_t len, void *u)
{
	struct scan_acc *a = u;

	if ((a->expect != 0 && lsn != a->expect) || wa <= a->last_wa ||
	    (lsn <= N_COMMITS && g_wa[lsn] != 0 &&
	    (wa != g_wa[lsn] || len != g_len[lsn])))
		a->bad = 1;
	(void)rec;
	a->expect = lsn + 1;
	a->last_wa = wa;
	a->count++;
	return 0;
}

static wal_t *
open_log(int append)
{
	wal_opts_t wo = { 0 };
	wal_t *w = NULL;

	wo.path = g_dir;
	wo.append = append;
	wo.segment_bytes = SEG_BYTES;
	CK(wal_open(&wo, &w) == XTC_OK);
	return w;
}

static void
run_group(int n_loops, const char *tag)
{
	struct scan_acc a = { 0, 0, 0, 0 };
	char rec[4096], got[4096];
	uint32_t lo = 0, cur = 0;
	long c;
	int k, bad = 0;

	memset(g_wa, 0, sizeof g_wa);
	g_wal = open_log(0);
	if (g_wal == NULL)
		return;
	xtc_atomic_store_i32(&g_left, N_COMMITTERS, xtc_mo_seq_cst);
	if (n_loops <= 1) {
		xtc_loop_t *loop = NULL;
		assert(xtc_loop_create(&loop) == XTC_OK);
		assert(wal_writer_spawn(g_wal, loop, NULL) == XTC_OK);
		for (c = 0; c < N_COMMITTERS; c++) {
			xtc_proc_opts_t po = { .size = sizeof(xtc_proc_opts_t),
			    .name = "cmt" };
			xtc_pid_t pid;
			assert(xtc_proc_spawn(loop, committer_proc, (void *)c,
			    &po, &pid) == XTC_OK);
		}
		assert(xtc_loop_run(loop) == XTC_OK);
		xtc_loop_destroy(loop);
	} else {
		xtc_exec_t *exec = NULL;
		assert(xtc_exec_create(&(xtc_exec_opts_t){
		    .size = sizeof(xtc_exec_opts_t), .n_loops = n_loops },
		    &exec) == XTC_OK);
		assert(wal_writer_spawn(g_wal, xtc_exec_loop(exec, 0), NULL) ==
		    XTC_OK);
		for (c = 0; c < N_COMMITTERS; c++) {
			xtc_proc_opts_t po = { .size = sizeof(xtc_proc_opts_t),
			    .name = "cmt" };
			xtc_pid_t pid;
			assert(xtc_proc_spawn(xtc_exec_loop(exec,
			    (int)(c % n_loops)), committer_proc, (void *)c, &po,
			    &pid) == XTC_OK);
		}
		assert(xtc_exec_run(exec) == XTC_OK);
		xtc_exec_destroy(exec);
	}
	CK(!xtc_atomic_load_i32(&g_fail_commit, xtc_mo_relaxed));
	wal_seg_range(g_wal, &lo, &cur);
	CK(lo == 1 && cur > 8);        /* ~4.6 MB in 64 KiB segments */
	/* Every address names its own body, inside one segment. */
	for (k = 1; k <= N_COMMITS; k++) {
		const char *p;
		long id;
		int i;
		if (g_wa[k] == 0 || !read_wa(g_wa[k], got, g_len[k])) {
			bad++;
			continue;
		}
		got[g_len[k] < sizeof got ? g_len[k] : sizeof got - 1] = 0;
		p = got;
		if (sscanf(p, "rec-%2ld-%4d:", &id, &i) != 2 ||
		    make_rec(rec, sizeof rec, id, i) != g_len[k] ||
		    memcmp(rec, got, g_len[k]) != 0 ||
		    WAL_WA_OFF(g_wa[k]) + g_len[k] + 8 > SEG_BYTES)
			bad++;
	}
	CK(bad == 0);
	wal_close(g_wal);
	CK(wal_scan_segs(g_dir, 0, scan_cb, &a) == XTC_OK);
	CK(a.count == N_COMMITS && !a.bad);
	printf("  group [%s]: %d commits in %u segments, every wa names its "
	    "body, scan in LSN order\n", tag, N_COMMITS, cur);
}

static void
t_big_and_release(void)
{
	struct scan_acc a = { 0, 0, 0, 0 };
	static char big[3u * 1024u * 1024u];
	uint64_t lsn, wa, was[40];
	uint32_t lo, cur, lo2, cur2, k;
	char p[300];
	struct stat sb;
	wal_t *w;
	int i;

	memset(g_wa, 0, sizeof g_wa);
	w = open_log(0);
	if (w == NULL)
		return;
	/* 40 records of 6 KB: about 10 per segment. */
	memset(big, 'z', sizeof big);
	for (i = 0; i < 40; i++)
		CK(wal_commit_wa(w, big, 6000, &lsn, &was[i]) == XTC_OK);
	/* A record larger than a segment, and one larger than a quarter of
	 * the ring (the direct path): each in a segment of its own. */
	CK(wal_commit_wa(w, big, 100000, &lsn, &wa) == XTC_OK);
	CK(WAL_WA_OFF(wa) == FIRST_OFF);
	seg_path(WAL_WA_SEG(wa), p, sizeof p);
	CK(stat(p, &sb) == 0);
	CK(wal_commit_wa(w, big, sizeof big, &lsn, &wa) == XTC_OK);
	CK(WAL_WA_OFF(wa) == FIRST_OFF);
	CK(wal_commit_wa(w, "after", 5, &lsn, &wa) == XTC_OK);
	CK(WAL_WA_OFF(wa) == FIRST_OFF);  /* the big one filled its segment */
	wal_seg_range(w, &lo, &cur);
	CK(lo == 1);
	/* Release below record 20's segment: the next roll unlinks only the
	 * segments before it, and the current one survives a too-high
	 * boundary. */
	wal_set_release(w, was[20]);
	wal_set_release(w, was[3]);    /* lower: ignored */
	CK(wal_roll(w) == XTC_OK);
	wal_seg_range(w, &lo2, &cur2);
	CK(lo2 == WAL_WA_SEG(was[20]) && cur2 == cur + 1);
	for (k = 1; k < lo2; k++) {
		seg_path(k, p, sizeof p);
		CK(stat(p, &sb) != 0);
	}
	seg_path(lo2, p, sizeof p);
	CK(stat(p, &sb) == 0);
	CK(wal_scan_segs(g_dir, was[0], scan_cb, &a) == XTC_E_NOTFOUND);
	CK(wal_scan_segs(g_dir, was[20], scan_cb, &a) == XTC_OK);
	CK(a.count == 40 - 20 + 3 && !a.bad);
	wal_set_release(w, WAL_WA(cur2 + 5, 0));
	CK(wal_roll(w) == XTC_OK);
	wal_seg_range(w, &lo2, &cur2);
	CK(lo2 == cur2);               /* never the current segment */
	wal_close(w);
	printf("  frames/release: a big frame gets its own segment; release "
	    "unlinks whole segments below it, oldest first\n");
}

static void
t_reopen(void)
{
	struct scan_acc a = { 0, 0, 0, 0 };
	uint64_t lsn = 0, wa = 0, wa1 = 0;
	uint32_t lo, cur;
	char p[300];
	wal_t *w;
	int fd, i;

	memset(g_wa, 0, sizeof g_wa);
	w = open_log(0);
	if (w == NULL)
		return;
	for (i = 0; i < 30; i++)
		CK(wal_commit_wa(w, "0123456789abcdef0123456789abcdef", 32,
		    &lsn, &wa) == XTC_OK);
	CK(wal_roll(w) == XTC_OK);
	for (i = 0; i < 5; i++)
		CK(wal_commit_wa(w, "second", 6, &lsn, &wa) == XTC_OK);
	wal_seg_range(w, &lo, &cur);
	wal_close(w);
	/* A torn last frame: a header that promises more than is there. */
	seg_path(cur, p, sizeof p);
	CK((fd = open(p, O_RDWR)) >= 0);
	CK(pwrite(fd, "\x24\0\0\0\0\0\0\0\x40\0\0\0junk", 16,
	    (off_t)WAL_WA_OFF(wa) + 6 + 8) == 16);
	close(fd);
	w = open_log(1);
	if (w == NULL)
		return;
	CK(wal_commit_wa(w, "third", 5, &lsn, &wa1) == XTC_OK);
	CK(lsn == 36);
	CK(WAL_WA_SEG(wa1) == cur && wa1 == wa + 6 + 8 + 12);
	wal_close(w);
	CK(wal_scan_segs(g_dir, 0, scan_cb, &a) == XTC_OK);
	CK(a.count == 36 && !a.bad);
	/* A newest segment with no header (a roll's create, then a crash):
	 * it never took a frame, so the open removes it. */
	seg_path(cur + 1, p, sizeof p);
	CK((fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0600)) >= 0);
	close(fd);
	w = open_log(1);
	if (w == NULL)
		return;
	CK(wal_commit_wa(w, "fourth", 6, &lsn, &wa) == XTC_OK);
	CK(lsn == 37 && WAL_WA_SEG(wa) == cur && wa == wa1 + 5 + 8 + 12);
	wal_close(w);
	printf("  reopen: append resumes after a torn tail and after a "
	    "headerless newest segment\n");
}

/*
 * The LSN outlives every segment that held a frame (review B1): with
 * the release at the log end, a roll deletes every segment with a
 * frame.  Both the clean path (roll, reopen, roll again, reopen) and a
 * crash right after the roll's unlink (nothing written after it) must
 * resume past the last LSN issued.
 */
static void
t_lsn(void)
{
	uint64_t lsn = 0, wa = 0;
	uint32_t lo, cur;
	wal_t *w;
	int i;

	w = open_log(0);
	if (w == NULL)
		return;
	for (i = 0; i < 10; i++)
		CK(wal_commit_wa(w, "abcdefgh", 8, &lsn, &wa) == XTC_OK);
	CK(lsn == 10);
	wal_set_release(w, wa + 20);
	CK(wal_roll(w) == XTC_OK);
	wal_seg_range(w, &lo, &cur);
	CK(lo == 1 && cur == 2);
	wal_close(w);
	w = open_log(1);
	if (w == NULL)
		return;
	/* Release the start of segment 2: the roll deletes segment 1, the
	 * only one with a frame.  wal_close writes nothing, so this is also
	 * a crash after the unlink and before segment 3's first frame. */
	wal_seg_range(w, &lo, &cur);
	wal_set_release(w, WAL_WA(cur, 0));
	CK(wal_roll(w) == XTC_OK);
	wal_seg_range(w, &lo, &cur);
	CK(lo == 2 && cur == 3);
	wal_close(w);
	w = open_log(1);
	if (w == NULL)
		return;
	CK(wal_commit_wa(w, "x", 1, &lsn, &wa) == XTC_OK);
	CK(lsn == 11 && WAL_WA_SEG(wa) == 3 && WAL_WA_OFF(wa) == FIRST_OFF);
	wal_close(w);
	printf("  lsn: no restart after every frame-holding segment is "
	    "released\n");
}

static int
count_cb(uint64_t lsn, uint64_t wa, const void *rec, uint32_t len, void *u)
{
	(void)lsn;
	(void)wa;
	(void)rec;
	(void)len;
	(*(int *)u)++;
	return 0;
}

/* The path of segment seg and the file offset of its last frame (each
 * frame of three_segs is 12 + 8 + 8 bytes): 0, or -1. */
static int
wal_commit_wa_last(uint32_t seg, char *p, size_t cap, off_t *off)
{
	struct stat sb;

	seg_path(seg, p, cap);
	if (stat(p, &sb) != 0)
		return -1;
	/* three_segs puts 10 frames in segment 2, from the header block. */
	*off = (off_t)WAL_SEG_HDR + 9 * (12 + 8 + 8);
	return 0;
}

/* 30 commits over three segments (10 in 1, 10 in 2, 10 in 3); *mid is
 * the WAL address of a record in segment 2. */
static void
three_segs(uint64_t *mid)
{
	uint64_t lsn = 0, wa = 0;
	wal_t *w;
	int i;

	w = open_log(0);
	if (w == NULL)
		return;
	for (i = 0; i < 30; i++) {
		CK(wal_commit_wa(w, "abcdefgh", 8, &lsn, &wa) == XTC_OK);
		if (i == 15)
			*mid = wa;
		if (i == 9 || i == 19)
			CK(wal_roll(w) == XTC_OK);
	}
	wal_close(w);
}

/*
 * The append open agrees with recovery (review B2): a frame of an older
 * segment damaged (probe p_gap's byte flip), or a middle segment gone,
 * refuses both, so no commit can be acknowledged past the point a scan
 * stops at.  The intact chain still opens and scans all 30.
 */
static void
t_chain(void)
{
	wal_opts_t wo = { 0 };
	uint64_t mid = 0;
	char p[300], q[310];
	uint8_t junk[64];
	off_t off = 0;
	wal_t *w = NULL;
	int fd, n = 0;

	three_segs(&mid);
	CK(WAL_WA_SEG(mid) == 2);
	CK(wal_scan_segs(g_dir, 0, count_cb, &n) == XTC_OK && n == 30);
	seg_path(2, p, sizeof p);
	CK((fd = open(p, O_RDWR)) >= 0);
	CK(pwrite(fd, "X", 1, (off_t)WAL_WA_OFF(mid)) == 1);
	close(fd);
	wo.path = g_dir;
	wo.append = 1;
	wo.segment_bytes = SEG_BYTES;
	CK(wal_open(&wo, &w) == XTC_E_IO);
	n = 0;
	CK(wal_scan_segs(g_dir, 0, count_cb, &n) == XTC_E_IO);
	/* A missing middle segment. */
	three_segs(&mid);
	seg_path(2, p, sizeof p);
	snprintf(q, sizeof q, "%s.away", p);
	CK(rename(p, q) == 0);
	CK(wal_open(&wo, &w) == XTC_E_IO);
	CK(wal_scan_segs(g_dir, 0, count_cb, &n) == XTC_E_IO);
	CK(rename(q, p) == 0);
	n = 0;
	CK(wal_scan_segs(g_dir, 0, count_cb, &n) == XTC_OK && n == 30);
	CK(wal_open(&wo, &w) == XTC_OK);
	wal_close(w);
	/* Segment 2's last frame zeroed: what follows its frames is clean,
	 * so only the LSN step to segment 3's header shows the loss. */
	three_segs(&mid);
	CK(wal_commit_wa_last(2, p, sizeof p, &off) == 0);
	CK((fd = open(p, O_RDWR)) >= 0);
	memset(junk, 0, sizeof junk);
	CK(pwrite(fd, junk, 12 + 8 + 8, off) == 12 + 8 + 8);
	close(fd);
	CK(wal_open(&wo, &w) == XTC_E_IO);
	CK(wal_scan_segs(g_dir, 0, count_cb, &n) == XTC_E_IO);
	/* Every frame of segment 2 intact, junk after them: only the
	 * clean-to-the-end check shows it. */
	three_segs(&mid);
	CK(wal_commit_wa_last(2, p, sizeof p, &off) == 0);
	CK((fd = open(p, O_RDWR)) >= 0);
	memset(junk, 0x5A, sizeof junk);
	CK(pwrite(fd, junk, sizeof junk, off + 12 + 8 + 8) ==
	    (ssize_t)sizeof junk);
	close(fd);
	CK(wal_open(&wo, &w) == XTC_E_IO);
	CK(wal_scan_segs(g_dir, 0, count_cb, &n) == XTC_E_IO);
	printf("  chain: a damaged, short or dirty older segment, or a missing "
	    "middle one, refuses the open and the scan\n");
}

static int
dump_one(wal_emit_fn emit, void *ctx, void *user)
{
	(void)user;
	emit(ctx, "x", 1);
	return XTC_OK;
}

static void
t_refuse(void)
{
	char one[300];
	wal_opts_t wo = { 0 };
	wal_ock_t *k = NULL;
	uint64_t lsn, wa = 99;
	wal_t *w;
	int fd;

	w = open_log(0);
	if (w == NULL)
		return;
	CK(wal_checkpoint(w, g_dir, dump_one, NULL) == XTC_E_INVAL);
	CK(wal_ockpt_begin(w, g_dir, &k) == XTC_E_INVAL);
	CK(wal_truncate(w) == XTC_E_INVAL);
	wal_close(w);
	t_tmpl(one, sizeof one, "walseg-one");
	CK((fd = mkstemp(one)) >= 0);
	close(fd);
	wo.path = one;
	CK(wal_open(&wo, &w) == XTC_OK);
	CK(wal_commit_wa(w, "a", 1, &lsn, &wa) == XTC_OK && wa == 0);
	CK(wal_roll(w) == XTC_E_INVAL);
	wal_close(w);
	unlink(one);
	printf("  refuse: no compaction or truncate of a segmented log; a "
	    "one-file log has no addresses\n");
}

static void
cleanup(void)
{
	char p[300];
	uint32_t k;

	for (k = 1; k < 4096; k++) {
		seg_path(k, p, sizeof p);
		(void)unlink(p);
	}
	(void)rmdir(g_dir);
}

int
main(void)
{
	t_tmpl(g_dir, sizeof g_dir, "walseg");
	if (mkdtemp(g_dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	run_group(1, "single-loop");
	run_group(4, "4-loop executor");
	t_big_and_release();
	t_reopen();
	t_lsn();
	t_chain();
	t_refuse();
	cleanup();
	if (!g_fail)
		printf("ok test_wal_seg\n");
	return g_fail;
}
