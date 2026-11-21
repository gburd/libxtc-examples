/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_onloop_io.c
 *	Nothing in the row log's framework blocks the OS thread of a loop
 *	(review S9): a fiber opens a segmented WAL, commits across rolls,
 *	opens a manifest, flushes runs, reopens and closes it, while this
 *	test interposes open, close, unlink, rename, fsync, fdatasync,
 *	opendir, pwrite, pread, ftruncate and mkdir and counts the calls
 *	made on the loop's own thread.  Each
 *	must be zero: the work goes through xtc_blocking_run (or xtc_aio)
 *	and runs on another thread.  wal_close, bm_create and bm_destroy
 *	are outside the watched window (start-up and shutdown).
 */

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "btnode.h"
#include "bufmgr.h"
#include "keydir.h"
#include "manifest.h"
#include "run.h"
#include "wal.h"
#include "xtc.h"
#include "xtc_atomic.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_rcu.h"
#include "t_tmp.h"

static pthread_t g_loop_thr;
static int       g_watch;
static int       g_hits[11];
static int       g_off;              /* watched calls on other threads */
static const char *const g_name[11] = { "open", "close", "unlink",
    "rename", "fsync", "fdatasync", "opendir", "pwrite", "ftruncate",
    "mkdir", "pread" };

/* Count a call made on the loop's thread while the window is open. */
static void
hit(int k)
{
	if (!xtc_atomic_load_i32(&g_watch, xtc_mo_acquire))
		return;
	if (pthread_equal(pthread_self(), g_loop_thr))
		(void)xtc_atomic_fetch_add_i32(&g_hits[k], 1, xtc_mo_relaxed);
	else
		(void)xtc_atomic_fetch_add_i32(&g_off, 1, xtc_mo_relaxed);
}

/* The next definition of `name` (dlsym returns an object pointer; ISO C
 * has no cast from it to a function pointer, so copy the bits). */
static void
next_sym(const char *name, void *fnp, size_t sz)
{
	void *sym = dlsym(RTLD_NEXT, name);

	memcpy(fnp, &sym, sz);
}

int
open(const char *path, int flags, ...)
{
	static int (*real)(const char *, int, ...);
	va_list ap;
	int mode = 0;

	if (real == NULL)
		next_sym("open", &real, sizeof real);
	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	hit(0);
	return real(path, flags, mode);
}

int
close(int fd)
{
	static int (*real)(int);

	if (real == NULL)
		next_sym("close", &real, sizeof real);
	hit(1);
	return real(fd);
}

int
unlink(const char *path)
{
	static int (*real)(const char *);

	if (real == NULL)
		next_sym("unlink", &real, sizeof real);
	hit(2);
	return real(path);
}

int
rename(const char *a, const char *b)
{
	static int (*real)(const char *, const char *);

	if (real == NULL)
		next_sym("rename", &real, sizeof real);
	hit(3);
	return real(a, b);
}

int
fsync(int fd)
{
	static int (*real)(int);

	if (real == NULL)
		next_sym("fsync", &real, sizeof real);
	hit(4);
	return real(fd);
}

int
fdatasync(int fd)
{
	static int (*real)(int);

	if (real == NULL)
		next_sym("fdatasync", &real, sizeof real);
	hit(5);
	return real(fd);
}

DIR *
opendir(const char *path)
{
	static DIR *(*real)(const char *);

	if (real == NULL)
		next_sym("opendir", &real, sizeof real);
	hit(6);
	return real(path);
}

ssize_t
pwrite(int fd, const void *b, size_t n, off_t o)
{
	static ssize_t (*real)(int, const void *, size_t, off_t);

	if (real == NULL)
		next_sym("pwrite", &real, sizeof real);
	hit(7);
	return real(fd, b, n, o);
}

ssize_t
pread(int fd, void *b, size_t n, off_t o)
{
	static ssize_t (*real)(int, void *, size_t, off_t);

	if (real == NULL)
		next_sym("pread", &real, sizeof real);
	hit(10);
	return real(fd, b, n, o);
}

int
ftruncate(int fd, off_t o)
{
	static int (*real)(int, off_t);

	if (real == NULL)
		next_sym("ftruncate", &real, sizeof real);
	hit(8);
	return real(fd, o);
}

int
mkdir(const char *p, mode_t m)
{
	static int (*real)(const char *, mode_t);

	if (real == NULL)
		next_sym("mkdir", &real, sizeof real);
	hit(9);
	return real(p, m);
}

static char g_dir[256];
static bm_t *g_bm;
static wal_t *g_wal;

static int
row_of(void *ctx, uint32_t frame, run_row_t *row)
{
	static char rec[16];

	(void)ctx;
	row->commit_ts = frame;
	row->self_wa = WAL_WA(1, frame);
	row->len = (uint16_t)snprintf(rec, sizeof rec, "f=%u", frame);
	row->rec = (const uint8_t *)rec;
	return XTC_OK;
}

static uint64_t
boundary(void *ctx)
{
	(void)ctx;
	return wal_end_wa(g_wal);
}

/* The fiber: every framework file operation, on the loop. */
static void
worker(void *arg)
{
	wal_opts_t wo = { 0 };
	manifest_t *m = NULL;
	keydir_t *kd = NULL;
	char p[300];
	uint64_t lsn, wa;
	uint32_t i, r;

	(void)arg;
	g_loop_thr = pthread_self();
	xtc_atomic_store_i32(&g_watch, 1, xtc_mo_release);
	snprintf(p, sizeof p, "%s/wal", g_dir);
	wo.path = p;
	wo.segment_bytes = 64u * 1024u;
	CK(wal_open(&wo, &g_wal) == XTC_OK);
	CK(kd_create(1024, &kd) == XTC_OK);
	CK(mf_open(g_dir, g_bm, kd, &m) == XTC_OK);
	if (g_wal == NULL || kd == NULL || m == NULL)
		goto out;
	for (r = 0; r < 4; r++) {
		for (i = 0; i < 100; i++) {
			CK(wal_commit_wa(g_wal, "commit", 6, &lsn, &wa) ==
			    XTC_OK);
			CK(kd_delta_put(kd, 1, (int64_t)(r * 100u + i),
			    r * 100u + i + 1u) == XTC_OK);
		}
		CK(mf_flush(m, row_of, NULL, boundary, NULL) == XTC_OK);
		wal_set_release(g_wal, mf_boundary(m));
		CK(wal_roll(g_wal) == XTC_OK);
	}
	mf_close(m);
	kd_destroy(kd);
	kd = NULL;
	CK(kd_create(1024, &kd) == XTC_OK);
	CK(mf_open(g_dir, g_bm, kd, &m) == XTC_OK && mf_nruns(m) == 4);
	mf_close(m);
out:
	xtc_atomic_store_i32(&g_watch, 0, xtc_mo_release);
	kd_destroy(kd);
}

int
main(void)
{
	bm_opts_t o = BM_OPTS_DEFAULT;
	xtc_proc_opts_t po = XTC_PROC_OPTS_INIT;
	xtc_loop_t *loop = NULL;
	xtc_pid_t pid;
	char p[300];
	int k, any = 0;

	t_tmpl(g_dir, sizeof g_dir, "onloop");
	if (mkdtemp(g_dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(p, sizeof p, "%s/pages", g_dir);
	o.path = p;
	o.page_size = 4096;
	o.n_frames = 64;
	o.cksum_off = BTNODE_CKSUM_OFF;
	o.cksum = wal_crc32c;
	CK(bm_create(&o, &g_bm) == XTC_OK);
	CK(xtc_loop_create(&loop) == XTC_OK);
	if (g_bm == NULL || loop == NULL)
		return 1;
	CK(xtc_proc_spawn(loop, worker, NULL, &po, &pid) == XTC_OK);
	CK(xtc_loop_run(loop) == XTC_OK);
	xtc_loop_destroy(loop);
	for (k = 0; k < 11; k++) {
		if (g_hits[k] != 0)
			printf("  %s on the loop thread: %d\n", g_name[k],
			    g_hits[k]);
		any |= g_hits[k];
	}
	CK(any == 0);
	CK(g_off > 20);                /* the interposer saw the work */
	wal_close(g_wal);
	bm_destroy(g_bm);
	xtc_rcu_fini();
	printf("  on-loop: no open/close/unlink/rename/fsync/fdatasync/"
	    "opendir/pwrite/pread/ftruncate/mkdir on the loop's thread "
	    "(%d off it)\n", g_off);
	if (!g_fail)
		printf("ok test_onloop_io\n");
	return g_fail;
}
