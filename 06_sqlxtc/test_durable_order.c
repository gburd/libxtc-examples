/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_durable_order.c
 *	The row log's fsync ordering, checked at every point where a crash
 *	would expose it.  The test interposes open, close, pwrite, pwritev,
 *	fsync, fdatasync, rename and unlink, and models what a crash keeps:
 *	a file's bytes are durable after an fsync or fdatasync of it (or at
 *	once on an O_DSYNC descriptor), and a created or renamed name after
 *	an fsync of its directory.  Then it checks:
 *
 *	  install    when MANIFEST.tmp is renamed over MANIFEST, the tmp
 *	             file's bytes are durable, and every run file in the
 *	             directory is durable with a durable name (a manifest
 *	             never names a run a crash could lose);
 *	  installed  when mf_flush returns, MANIFEST's rename is durable;
 *	  retention  when a WAL segment is unlinked, a newer segment exists
 *	             with a durable name and a durable header (the LSN
 *	             survives: STORAGE_DESIGN 6.2, review B1);
 *	  commit     when wal_commit_wa returns, the segment holding the
 *	             record is durable with a durable name.
 *
 *	The workload commits across many segment rolls, flushes the
 *	nursery into runs, releases the WAL below each flush boundary and
 *	reopens both.  It runs off a loop, where xtc_aio and
 *	xtc_blocking_run run inline, so every operation is seen in order.
 */

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "btnode.h"
#include "bufmgr.h"
#include "keydir.h"
#include "manifest.h"
#include "run.h"
#include "wal.h"
#include "xtc.h"
#include "xtc_rcu.h"
#include "t_tmp.h"

static char g_dir[256];

/* ---- the crash model ---- */

#define NPATH 512
#define NFD   1024

struct tpath {
	char path[300];
	int  used;
	int  dirty;                  /* bytes written since its last sync */
	int  name_pending;           /* created/renamed, dir not synced */
};

struct tfd {
	int pi;                      /* tracked path, or -1 */
	int dsync;                   /* O_DSYNC: every write is durable */
	int isdir;
	char dir[300];               /* isdir: the directory */
};

static struct tpath g_p[NPATH];
static struct tfd   g_fd[NFD];
static int          g_on;        /* the model is armed */
static int          g_bad, g_installs, g_unlinks, g_commits;

static int (*r_open)(const char *, int, ...);
static int (*r_close)(int);
static ssize_t (*r_pwrite)(int, const void *, size_t, off_t);
static ssize_t (*r_pwritev)(int, const struct iovec *, int, off_t);
static int (*r_fsync)(int);
static int (*r_fdatasync)(int);
static int (*r_rename)(const char *, const char *);
static int (*r_unlink)(const char *);

/* The next definition of `name` (dlsym returns an object pointer; ISO C
 * has no cast from it to a function pointer, so copy the bits). */
static void
next_sym(const char *name, void *fnp, size_t sz)
{
	void *sym = dlsym(RTLD_NEXT, name);

	memcpy(fnp, &sym, sz);
}

static void
init_syms(void)
{
	if (r_open != NULL)
		return;
	next_sym("open", &r_open, sizeof r_open);
	next_sym("close", &r_close, sizeof r_close);
	next_sym("pwrite", &r_pwrite, sizeof r_pwrite);
	next_sym("pwritev", &r_pwritev, sizeof r_pwritev);
	next_sym("fsync", &r_fsync, sizeof r_fsync);
	next_sym("fdatasync", &r_fdatasync, sizeof r_fdatasync);
	next_sym("rename", &r_rename, sizeof r_rename);
	next_sym("unlink", &r_unlink, sizeof r_unlink);
}

static int
in_dir(const char *path)
{
	size_t n = strlen(g_dir);

	return g_dir[0] != '\0' && strncmp(path, g_dir, n) == 0 &&
	    (path[n] == '/' || path[n] == '\0');
}

static int
path_find(const char *path)
{
	int i;

	for (i = 0; i < NPATH; i++)
		if (g_p[i].used && strcmp(g_p[i].path, path) == 0)
			return i;
	return -1;
}

static int
path_add(const char *path)
{
	int i = path_find(path);

	if (i >= 0)
		return i;
	for (i = 0; i < NPATH; i++)
		if (!g_p[i].used) {
			memset(&g_p[i], 0, sizeof g_p[i]);
			g_p[i].used = 1;
			snprintf(g_p[i].path, sizeof g_p[i].path, "%s", path);
			return i;
		}
	return -1;
}

/* The directory part of path, into out. */
static void
dir_of(const char *path, char *out, size_t cap)
{
	const char *s = strrchr(path, '/');
	size_t n = s == NULL ? 0 : (size_t)(s - path);

	if (n >= cap)
		n = cap - 1;
	memcpy(out, path, n);
	out[n] = '\0';
}

static void
violation(const char *what, const char *path)
{
	if (g_bad < 8)
		fprintf(stderr, "  ORDER: %s: %s\n", what, path);
	g_bad++;
}

/* 1 if path's bytes and name are both durable. */
static int
durable(const char *path)
{
	int i = path_find(path);

	return i < 0 || (!g_p[i].dirty && !g_p[i].name_pending);
}

/* The basename's number for "<prefix><n><suffix>" files, or 0. */
static unsigned
file_num(const char *path, const char *fmt)
{
	const char *b = strrchr(path, '/');
	unsigned v = 0;
	char tail;

	b = b == NULL ? path : b + 1;
	if (sscanf(b, fmt, &v, &tail) != 2)
		return 0;
	return v;
}

/* The retention check (see the header) before unlink(path). */
static void
check_seg_unlink(const char *path)
{
	unsigned k = file_num(path, "wal-%8u.lo%c"), n, best = 0;
	char d[300], newer[320], magic[8];
	DIR *dp;
	struct dirent *e;
	int fd;

	if (k == 0)
		return;
	g_unlinks++;
	dir_of(path, d, sizeof d);
	if ((dp = opendir(d)) == NULL)
		return;
	while ((e = readdir(dp)) != NULL)
		if ((n = file_num(e->d_name, "wal-%8u.lo%c")) > best)
			best = n;
	closedir(dp);
	if (best <= k) {
		violation("segment unlinked with no newer segment", path);
		return;
	}
	snprintf(newer, sizeof newer, "%s/wal-%08u.log", d, best);
	if (!durable(newer))
		violation("segment unlinked before the newest segment's "
		    "header and name are durable", newer);
	if ((fd = r_open(newer, O_RDONLY)) >= 0) {
		if (pread(fd, magic, 8, 0) != 8 ||
		    memcmp(magic, "SXWALSEG", 8) != 0)
			violation("segment unlinked while the newest has no "
			    "header", newer);
		r_close(fd);
	}
}

/* The install check (see the header) before rename(tmp, MANIFEST). */
static void
check_install(const char *from)
{
	char d[300];
	int i;

	g_installs++;
	if (!durable(from) && g_p[path_find(from)].dirty)
		violation("MANIFEST.tmp renamed before it is durable", from);
	dir_of(from, d, sizeof d);
	for (i = 0; i < NPATH; i++) {
		char pd[300];
		if (!g_p[i].used ||
		    file_num(g_p[i].path, "rl-%u.ru%c") == 0)
			continue;
		dir_of(g_p[i].path, pd, sizeof pd);
		if (strcmp(pd, d) == 0 && g_p[i].dirty)
			violation("manifest installed over a run whose bytes "
			    "are not durable", g_p[i].path);
		else if (strcmp(pd, d) == 0 && g_p[i].name_pending)
			violation("manifest installed over a run whose name "
			    "is not durable", g_p[i].path);
	}
}

/* ---- the interposers ---- */

int
open(const char *path, int flags, ...)
{
	struct stat sb;
	va_list ap;
	int mode = 0, fd, existed, pi;

	init_syms();
	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	existed = stat(path, &sb) == 0;
	fd = r_open(path, flags, mode);
	if (fd < 0 || fd >= NFD || !g_on || !in_dir(path))
		return fd;
	memset(&g_fd[fd], 0, sizeof g_fd[fd]);
	g_fd[fd].pi = -1;
	if (stat(path, &sb) == 0 && S_ISDIR(sb.st_mode)) {
		g_fd[fd].isdir = 1;
		snprintf(g_fd[fd].dir, sizeof g_fd[fd].dir, "%s", path);
		return fd;
	}
	pi = path_add(path);
	g_fd[fd].pi = pi;
#if defined(O_DSYNC)
	g_fd[fd].dsync = (flags & O_DSYNC) != 0;
#endif
	if (pi >= 0 && (!existed || (flags & O_TRUNC)))
		g_p[pi].dirty = 1;   /* a new or emptied file: not durable */
	if (pi >= 0 && !existed)
		g_p[pi].name_pending = 1;
	return fd;
}

int
close(int fd)
{
	init_syms();
	if (fd >= 0 && fd < NFD) {
		g_fd[fd].pi = -1;
		g_fd[fd].isdir = 0;
	}
	return r_close(fd);
}

/* A write: the file's bytes are not durable, unless the descriptor is
 * O_DSYNC, whose write is durable (with what it takes to read it back)
 * when it returns.  The model keeps one dirty bit per file, so it takes
 * a dsync write as syncing the file: in the row log every write to a
 * dsync file goes through its one dsync descriptor. */
static void
wrote(int fd, ssize_t n)
{
	if (fd >= 0 && fd < NFD && g_fd[fd].pi >= 0)
		g_p[g_fd[fd].pi].dirty = !g_fd[fd].dsync || n < 0;
}

ssize_t
pwrite(int fd, const void *buf, size_t n, off_t off)
{
	ssize_t got;

	init_syms();
	got = r_pwrite(fd, buf, n, off);
	wrote(fd, got);
	return got;
}

ssize_t
pwritev(int fd, const struct iovec *iov, int cnt, off_t off)
{
	ssize_t got;

	init_syms();
	got = r_pwritev(fd, iov, cnt, off);
	wrote(fd, got);
	return got;
}

/* An fsync: a file's bytes are durable; a directory's names are. */
static void
synced(int fd)
{
	char pd[300];
	int i;

	if (fd < 0 || fd >= NFD)
		return;
	if (g_fd[fd].pi >= 0) {
		g_p[g_fd[fd].pi].dirty = 0;
		return;
	}
	if (!g_fd[fd].isdir)
		return;
	for (i = 0; i < NPATH; i++) {
		if (!g_p[i].used)
			continue;
		dir_of(g_p[i].path, pd, sizeof pd);
		if (strcmp(pd, g_fd[fd].dir) == 0)
			g_p[i].name_pending = 0;
	}
}

int
fsync(int fd)
{
	int rc;

	init_syms();
	if ((rc = r_fsync(fd)) == 0)
		synced(fd);
	return rc;
}

int
fdatasync(int fd)
{
	int rc;

	init_syms();
	if ((rc = r_fdatasync(fd)) == 0 && fd >= 0 && fd < NFD &&
	    g_fd[fd].pi >= 0)
		g_p[g_fd[fd].pi].dirty = 0;
	return rc;
}

int
rename(const char *from, const char *to)
{
	int a, b, rc;

	init_syms();
	if (g_on && in_dir(from) && strstr(from, "MANIFEST.tmp") != NULL)
		check_install(from);
	if ((rc = r_rename(from, to)) != 0 || !g_on || !in_dir(to))
		return rc;
	a = path_find(from);
	b = path_add(to);
	if (b >= 0) {
		g_p[b].dirty = a >= 0 ? g_p[a].dirty : 0;
		g_p[b].name_pending = 1;
	}
	if (a >= 0)
		g_p[a].used = 0;
	return rc;
}

int
unlink(const char *path)
{
	int i, rc;

	init_syms();
	if (g_on && in_dir(path))
		check_seg_unlink(path);
	if ((rc = r_unlink(path)) == 0 && (i = path_find(path)) >= 0)
		g_p[i].used = 0;
	return rc;
}

/* ---- the workload ---- */

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

static wal_t *
open_wal(int append)
{
	wal_opts_t wo = { 0 };
	char p[300];
	wal_t *w = NULL;

	snprintf(p, sizeof p, "%s/wal", g_dir);
	wo.path = p;
	wo.append = append;
	wo.segment_bytes = 64u * 1024u;
	CK(wal_open(&wo, &w) == XTC_OK);
	return w;
}

/* One commit, then the commit check. */
static void
commit(uint32_t i, keydir_t *kd)
{
	static char rec[3000];
	uint64_t lsn = 0, wa = 0;
	uint32_t len = 1u + (i * 977u) % (uint32_t)sizeof rec;
	char p[300];

	memset(rec, 'a' + (int)(i % 26u), len);
	CK(wal_commit_wa(g_wal, rec, len, &lsn, &wa) == XTC_OK);
	CK(kd_delta_put(kd, 1, (int64_t)i, i + 1u) == XTC_OK);
	snprintf(p, sizeof p, "%s/wal/wal-%08u.log", g_dir, WAL_WA_SEG(wa));
	g_commits++;
	if (!durable(p))
		violation("commit acknowledged before its segment is "
		    "durable", p);
}

static void
run_rounds(manifest_t *m, keydir_t *kd, uint32_t *next)
{
	char p[300];
	uint32_t r, i;

	snprintf(p, sizeof p, "%s/MANIFEST", g_dir);
	for (r = 0; r < 6; r++) {
		for (i = 0; i < 120; i++)
			commit((*next)++, kd);
		CK(mf_flush(m, row_of, NULL, boundary, NULL) == XTC_OK);
		if (!durable(p))
			violation("mf_flush returned before MANIFEST's rename "
			    "is durable", p);
		wal_set_release(g_wal, mf_boundary(m));
		CK(wal_roll(g_wal) == XTC_OK);
	}
}

/* Remove every file under dir (one level of subdirectory), then dir. */
static void
cleanup(const char *dir)
{
	char p[600];
	struct dirent *e;
	DIR *dp;

	if ((dp = opendir(dir)) == NULL)
		return;
	while ((e = readdir(dp)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		if (r_unlink(p) != 0)
			cleanup(p);
	}
	closedir(dp);
	(void)rmdir(dir);
}

int
main(void)
{
	bm_opts_t o = BM_OPTS_DEFAULT;
	manifest_t *m = NULL;
	keydir_t *kd = NULL;
	bm_t *bm = NULL;
	char p[300];
	uint32_t next = 0;

	init_syms();
	t_tmpl(g_dir, sizeof g_dir, "durorder");
	if (mkdtemp(g_dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	g_on = 1;
	snprintf(p, sizeof p, "%s/pages", g_dir);
	o.path = p;
	o.page_size = 4096;
	o.n_frames = 64;
	o.cksum_off = BTNODE_CKSUM_OFF;
	o.cksum = wal_crc32c;
	CK(bm_create(&o, &bm) == XTC_OK);
	CK(kd_create(4096, &kd) == XTC_OK);
	g_wal = open_wal(0);
	CK(bm != NULL && kd != NULL && g_wal != NULL &&
	    mf_open(g_dir, bm, kd, &m) == XTC_OK);
	if (m == NULL || g_wal == NULL)
		return 1;
	run_rounds(m, kd, &next);
	/* Reopen both and go on: the open paths keep the order too. */
	mf_close(m);
	kd_destroy(kd);
	wal_close(g_wal);
	CK(kd_create(4096, &kd) == XTC_OK);
	g_wal = open_wal(1);
	CK(kd != NULL && g_wal != NULL &&
	    mf_open(g_dir, bm, kd, &m) == XTC_OK);
	if (m == NULL || g_wal == NULL)
		return 1;
	run_rounds(m, kd, &next);
	mf_close(m);
	kd_destroy(kd);
	wal_close(g_wal);
	bm_destroy(bm);
	g_on = 0;
	CK(g_bad == 0);
	CK(g_installs >= 13 && g_unlinks >= 6 && g_commits == 1440);
	printf("  durable order: %d commits, %d installs, %d segment unlinks; "
	    "%d ordering violations\n", g_commits, g_installs, g_unlinks,
	    g_bad);
	cleanup(g_dir);
	xtc_rcu_fini();
	if (!g_fail)
		printf("ok test_durable_order\n");
	return g_fail;
}
