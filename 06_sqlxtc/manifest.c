/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/manifest.c
 *	The MANIFEST and the nursery flush.  See manifest.h.
 *
 *	One flusher at a time: the caller serializes mf_flush (the merge
 *	lock of STORAGE_DESIGN 9.5).  The manifest's state is that
 *	flusher's; readers see only the keydir's RCU-published run set.
 */

#include "manifest.h"

#include "wal.h"            /* wal_crc32c */
#include "xtc.h"
#include "xtc_async.h"
#include "xtc_blocking.h"
#include "xtc_fs.h"
#include "xtc_rcu.h"
#include "xtc_sim.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MF_NAME   "MANIFEST"
#define MF_TMP    "MANIFEST.tmp"
#define MF_MAGIC  "sqlxtc-manifest 1\n"
#define MF_MAXLEN (1u << 20)

struct mf_run {
	run_meta_t meta;
	run_t     *run;
};

struct manifest {
	char          *dir;
	bm_t          *bm;
	keydir_t      *kd;
	uint64_t       next;          /* next run id */
	uint64_t       boundary;      /* of the durable manifest */
	uint64_t       frz_boundary;  /* of the frozen delta */
	struct mf_run *runs;          /* oldest first */
	uint32_t       n, cap;
	int            failed;        /* fail-stopped: see mf_flush */
};

/*
 * DST pessimal point (STORAGE_DESIGN 11.3): under the simulator with
 * buggify on, yield here on a seeded coin, so a kill or a concurrent
 * commit lands in the window.  A relaxed load in production.
 */
static void
mf_buggify(const char *site)
{
	if (XTC_SIM_BUGGIFY(site) && xtc_sim_buggify_fault(250) &&
	    !xtc_pid_is_none(xtc_self()))
		xtc_yield();
}

/* ---- file work, off the loop ---- */

struct mf_job {
	const char *dir;
	const char *text;             /* write: the new manifest */
	size_t      len;
	char       *out;              /* read: malloc'd contents, or NULL */
	size_t      outlen;
	int         op;               /* 0 read, 1 write, 2 unlink, 3 list */
	const char *name;             /* 2: the file in dir to unlink */
	char      **names;            /* 3: out, malloc'd, nnames of them */
	uint32_t    nnames;
	int         renamed;          /* 1: out, the rename was done */
};

/* The directory fsync after a manifest's rename; a test replaces it to
 * make that step fail. */
#ifndef MF_RENAMED_SYNC
#define MF_RENAMED_SYNC(dir) mf_sync_dir(dir)
#endif

/* fsync directory `dir` so a rename or a create in it is durable. */
static int
mf_sync_dir(const char *dir)
{
	int fd, rc;

	if ((rc = xtc_fs_open(dir, XTC_FS_READ, &fd)) != XTC_OK)
		return rc;
	rc = xtc_fs_fsync(fd);
	(void)xtc_fs_close(fd);
	return rc;
}

static int
mf_read_file(struct mf_job *j)
{
	char p[1100];
	int64_t sz = 0;
	size_t got = 0;
	int fd, rc;

	snprintf(p, sizeof p, "%s/%s", j->dir, MF_NAME);
	j->out = NULL;
	/* Opened directly: only ENOENT is "absent"; EACCES, EIO and the
	 * rest are errors, never a new store. */
	if ((rc = xtc_fs_open(p, XTC_FS_READ, &fd)) != XTC_OK)
		return rc == XTC_E_NOTFOUND ? rc : XTC_E_IO;
	rc = xtc_fs_fsize(fd, &sz);
	if (rc == XTC_OK && (sz <= 0 || (uint64_t)sz > MF_MAXLEN))
		rc = XTC_E_IO;
	if (rc == XTC_OK && (j->out = malloc((size_t)sz + 1u)) == NULL)
		rc = XTC_E_NOMEM;
	if (rc == XTC_OK &&
	    (rc = xtc_fs_pread(fd, j->out, (size_t)sz, 0, &got)) == XTC_OK &&
	    got != (size_t)sz)
		rc = XTC_E_IO;
	(void)xtc_fs_close(fd);
	if (rc != XTC_OK) {
		free(j->out);
		j->out = NULL;
		return rc;
	}
	j->out[sz] = '\0';
	j->outlen = (size_t)sz;
	return XTC_OK;
}

/* MANIFEST.tmp written and fsynced, renamed over MANIFEST, the
 * directory fsynced.  j->renamed says whether the rename was done: past
 * it the old manifest is gone, so a later failure (the directory fsync)
 * leaves the new one visible and perhaps durable. */
static int
mf_write_file(struct mf_job *j)
{
	char tmp[1100], fin[1100];
	size_t done = 0;
	int fd, rc;

	snprintf(tmp, sizeof tmp, "%s/%s", j->dir, MF_TMP);
	snprintf(fin, sizeof fin, "%s/%s", j->dir, MF_NAME);
	if ((rc = xtc_fs_open(tmp, XTC_FS_WRITE | XTC_FS_CREATE |
	    XTC_FS_TRUNC, &fd)) != XTC_OK)
		return rc;
	rc = xtc_fs_pwrite(fd, j->text, j->len, 0, &done);
	if (rc == XTC_OK && done != j->len)
		rc = XTC_E_IO;
	if (rc == XTC_OK)
		rc = xtc_fs_fsync(fd);
	(void)xtc_fs_close(fd);
	if (rc == XTC_OK)
		rc = xtc_fs_rename(tmp, fin);
	if (rc != XTC_OK) {
		(void)xtc_fs_unlink(tmp);
		return rc;
	}
	j->renamed = 1;
	return MF_RENAMED_SYNC(j->dir);
}

/* The names of the row log's files in j->dir (rl-*.run and
 * MANIFEST.tmp) into j->names. */
static int
mf_list_dir(struct mf_job *j)
{
	xtc_fs_dir_t *d = NULL;
	const char *name;
	char **nv;
	uint64_t id;
	char tail;
	int rc;

	j->names = NULL;
	j->nnames = 0;
	if ((rc = xtc_fs_dir_open(j->dir, &d)) != XTC_OK)
		return XTC_E_IO;
	while ((rc = xtc_fs_dir_next(d, &name)) == XTC_OK && name != NULL) {
		if (strcmp(name, MF_TMP) != 0 &&
		    (sscanf(name, "rl-%" SCNu64 ".ru%c", &id, &tail) != 2 ||
		    tail != 'n'))
			continue;
		if ((nv = realloc(j->names, (j->nnames + 1u) * sizeof *nv)) ==
		    NULL || (nv[j->nnames] = strdup(name)) == NULL) {
			if (nv != NULL)
				j->names = nv;
			rc = XTC_E_NOMEM;
			break;
		}
		j->names = nv;
		j->nnames++;
	}
	xtc_fs_dir_close(d);
	return rc;
}

static void
mf_names_free(struct mf_job *j)
{
	uint32_t i;

	for (i = 0; i < j->nnames; i++)
		free(j->names[i]);
	free(j->names);
	j->names = NULL;
	j->nnames = 0;
}

static int
mf_job_run(void *arg)
{
	struct mf_job *j = arg;
	char p[1100];

	if (j->op == 0)
		return mf_read_file(j);
	if (j->op == 1)
		return mf_write_file(j);
	if (j->op == 3)
		return mf_list_dir(j);
	snprintf(p, sizeof p, "%s/%s", j->dir, j->name);
	return xtc_fs_unlink(p);
}

/* Run a job through xtc_blocking_run (inline off a loop). */
static int
mf_job(struct mf_job *j)
{
	int rc, res = XTC_E_IO;

	while ((rc = xtc_blocking_run(mf_job_run, j, &res)) == XTC_E_AGAIN)
		xtc_yield();
	return rc != XTC_OK ? rc : res;
}

/* ---- text form ---- */

/* Bytes a run line can take: "run " + 20 digits + " " + 10 + " " + 10
 * + " " + 20 + " " + 8 hex + "\n" is 77; the head and the CRC line fit
 * in MF_HEAD_MAX.  A line that does not fit is a bug, never a line
 * silently left out (the next open would sweep its run). */
#define MF_LINE_MAX 96u
#define MF_HEAD_MAX 128u

/* Append one formatted piece; 0 if it did not fit. */
static int
mf_put(size_t cap, size_t *at, size_t n)
{
	if (n == 0 || n >= cap - *at)
		return 0;
	*at += n;
	return 1;
}

/* The manifest text for runs[0..n) (and one more, `add`, if not NULL),
 * next and boundary: XTC_OK with *out (malloc'd) and *len, else
 * XTC_E_NOMEM or XTC_E_INTERNAL (a line did not fit). */
static int
mf_text(const manifest_t *m, const run_meta_t *add, uint64_t next,
    uint64_t boundary, char **out, size_t *len)
{
	size_t cap = 2u * MF_HEAD_MAX + MF_LINE_MAX * ((size_t)m->n + 1u);
	size_t at = 0;
	const run_meta_t *r;
	char *t;
	uint32_t i;
	int n, ok;

	*out = NULL;
	if ((t = malloc(cap)) == NULL)
		return XTC_E_NOMEM;
	n = snprintf(t, cap, "%snext %" PRIu64 "\nboundary %016" PRIx64 "\n",
	    MF_MAGIC, next, boundary);
	ok = n > 0 && mf_put(cap, &at, (size_t)n);
	for (i = 0; ok && i <= m->n; i++) {
		r = i < m->n ? &m->runs[i].meta : add;
		if (r != NULL)
			ok = mf_put(cap, &at, run_meta_enc(r, t + at,
			    cap - at));
	}
	if (ok) {
		n = snprintf(t + at, cap - at, "crc %08x\n",
		    wal_crc32c(0, t, at));
		ok = n > 0 && mf_put(cap, &at, (size_t)n);
	}
	if (!ok) {
		free(t);
		return XTC_E_INTERNAL;
	}
	*out = t;
	*len = at;
	return XTC_OK;
}

/* Parse the manifest text: XTC_OK with next, boundary and the run lines
 * (malloc'd, *n of them), or XTC_E_IO. */
static int
mf_parse(char *t, size_t len, uint64_t *next, uint64_t *boundary,
    run_meta_t **out, uint32_t *n)
{
	char *crc_at, *line, *nl;
	unsigned crc;
	run_meta_t *v = NULL, *nv;
	uint32_t k = 0, cap = 0;

	*out = NULL;
	*n = 0;
	/* The CRC line is last and covers everything before it. */
	if (len < sizeof MF_MAGIC || t[len - 1] != '\n')
		return XTC_E_IO;
	t[len - 1] = '\0';
	if ((crc_at = strrchr(t, '\n')) == NULL ||
	    sscanf(crc_at + 1, "crc %8x", &crc) != 1 ||
	    wal_crc32c(0, t, (size_t)(crc_at + 1 - t)) != crc ||
	    strncmp(t, MF_MAGIC, sizeof MF_MAGIC - 1u) != 0)
		return XTC_E_IO;
	crc_at[1] = '\0';
	line = t + sizeof MF_MAGIC - 1u;
	if (sscanf(line, "next %" SCNu64 "\nboundary %" SCNx64, next,
	    boundary) != 2)
		return XTC_E_IO;
	/* Past the next and boundary lines (each ends in a newline). */
	if ((line = strchr(line, '\n')) == NULL ||
	    (line = strchr(line + 1, '\n')) == NULL)
		return XTC_E_IO;
	line++;
	for (; *line != '\0'; line = nl + 1) {
		if ((nl = strchr(line, '\n')) == NULL)
			break;
		if (k == cap) {
			cap = cap ? cap * 2u : 16u;
			if ((nv = realloc(v, cap * sizeof *v)) == NULL) {
				free(v);
				return XTC_E_NOMEM;
			}
			v = nv;
		}
		if (!run_meta_dec(line, &v[k]) || v[k].runid >= *next) {
			free(v);
			return XTC_E_IO;
		}
		k++;
	}
	*out = v;
	*n = k;
	return XTC_OK;
}

/* ---- the run set ---- */

static int
mf_push(manifest_t *m, const run_meta_t *meta, run_t *r)
{
	struct mf_run *nr;

	if (m->n == m->cap) {
		uint32_t c = m->cap ? m->cap * 2u : 16u;
		if ((nr = xtc_realloc(m->runs, c * sizeof *nr)) == NULL)
			return XTC_E_NOMEM;
		m->runs = nr;
		m->cap = c;
	}
	m->runs[m->n].meta = *meta;
	m->runs[m->n].run = r;
	m->n++;
	return XTC_OK;
}

/* Publish the run set in the keydir, newest first. */
static int
mf_publish(manifest_t *m)
{
	run_t **v;
	uint32_t i;
	int rc;

	if ((v = xtc_malloc((m->n + 1u) * sizeof *v)) == NULL)
		return XTC_E_NOMEM;
	for (i = 0; i < m->n; i++)
		v[i] = m->runs[m->n - 1u - i].run;
	rc = kd_runs_set(m->kd, v, m->n);
	xtc_free(v);
	return rc;
}

/* 1 if run id `id` would share a buffer-manager file number with a
 * live run (ids 65535 apart). */
static int
mf_file_busy(const manifest_t *m, uint64_t id)
{
	uint32_t i;

	for (i = 0; i < m->n; i++)
		if (RUN_FILE(m->runs[i].meta.runid) == RUN_FILE(id))
			return 1;
	return 0;
}

/* Delete rl-*.run files of `dir` that the durable manifest (m, just
 * read) does not name, and a stale MANIFEST.tmp: a flush that died
 * before its rename left them, and nothing durable refers to them (a
 * flush's boundary reaches the WAL only after mf_flush returns XTC_OK).
 * The directory walk and the unlinks run under xtc_blocking_run. */
static void
mf_sweep(manifest_t *m)
{
	struct mf_job j, u;
	uint32_t i, k;
	uint64_t id;
	char tail;
	int named;

	memset(&j, 0, sizeof j);
	j.dir = m->dir;
	j.op = 3;
	if (mf_job(&j) != XTC_OK) {
		mf_names_free(&j);
		return;
	}
	for (i = 0; i < j.nnames; i++) {
		named = 0;
		if (sscanf(j.names[i], "rl-%" SCNu64 ".ru%c", &id, &tail) ==
		    2)
			for (k = 0; k < m->n && !named; k++)
				named = m->runs[k].meta.runid == id;
		if (named)
			continue;
		memset(&u, 0, sizeof u);
		u.dir = m->dir;
		u.op = 2;
		u.name = j.names[i];
		(void)mf_job(&u);
	}
	mf_names_free(&j);
}

/*
 * No MANIFEST in `dir`: a new store, unless the directory holds run
 * files, which only a store that had a manifest can have (a store's
 * first manifest is written when it is created, before any flush).
 * Then MANIFEST was lost or removed, and opening it as a new store
 * would delete every flushed row: refuse, naming the problem.  A lone
 * MANIFEST.tmp is a create that crashed before its rename.  On a new
 * store, the empty manifest (next 1, boundary 0) is installed now.
 */
static int
mf_create(manifest_t *m)
{
	struct mf_job j;
	uint32_t i, runs = 0;
	size_t len;
	char *text;
	int rc;

	memset(&j, 0, sizeof j);
	j.dir = m->dir;
	j.op = 3;
	rc = mf_job(&j);
	for (i = 0; rc == XTC_OK && i < j.nnames; i++)
		runs += strcmp(j.names[i], MF_TMP) != 0;
	mf_names_free(&j);
	if (rc != XTC_OK)
		return rc;
	if (runs > 0) {
		fprintf(stderr, "sqlxtc row log: %s/%s is missing but the "
		    "directory holds %u run file%s: the store is damaged and "
		    "is not opened\n", m->dir, MF_NAME, runs, runs == 1 ? "" :
		    "s");
		return XTC_E_IO;
	}
	if ((rc = mf_text(m, NULL, m->next, m->boundary, &text, &len)) !=
	    XTC_OK)
		return rc;
	memset(&j, 0, sizeof j);
	j.dir = m->dir;
	j.text = text;
	j.len = len;
	j.op = 1;
	rc = mf_job(&j);
	free(text);
	return rc;
}

int
mf_open(const char *dir, bm_t *bm, keydir_t *kd, manifest_t **out)
{
	struct mf_job j;
	run_meta_t *v = NULL;
	char p[1100];
	uint32_t n = 0, i;
	manifest_t *m;
	run_t *r;
	int rc;

	if (dir == NULL || bm == NULL || kd == NULL || out == NULL)
		return XTC_E_INVAL;
	if ((m = xtc_calloc(1, sizeof *m)) == NULL)
		return XTC_E_NOMEM;
	m->bm = bm;
	m->kd = kd;
	m->next = 1;
	if ((m->dir = strdup(dir)) == NULL) {
		xtc_free(m);
		return XTC_E_NOMEM;
	}
	memset(&j, 0, sizeof j);
	j.dir = dir;
	j.op = 0;
	rc = mf_job(&j);
	if (rc == XTC_OK) {
		rc = mf_parse(j.out, j.outlen, &m->next, &m->boundary, &v, &n);
		free(j.out);
	} else if (rc == XTC_E_NOTFOUND) {
		rc = mf_create(m);
	} else {
		fprintf(stderr, "sqlxtc row log: %s/%s cannot be read: the "
		    "store is not opened\n", dir, MF_NAME);
	}
	/* Every named run must open, or the store is not usable. */
	for (i = 0; rc == XTC_OK && i < n; i++) {
		run_path(dir, v[i].runid, p, sizeof p);
		if ((rc = run_open(bm, p, &v[i], &r)) != XTC_OK)
			break;
		if ((rc = mf_push(m, &v[i], r)) != XTC_OK)
			run_close(r);
	}
	free(v);
	if (rc == XTC_OK)
		rc = mf_publish(m);
	if (rc != XTC_OK) {
		mf_close(m);
		return rc == XTC_E_NOMEM ? rc : XTC_E_IO;
	}
	mf_sweep(m);
	*out = m;
	return XTC_OK;
}

void
mf_close(manifest_t *m)
{
	uint32_t i;

	if (m == NULL)
		return;
	(void)kd_runs_set(m->kd, NULL, 0);
	for (i = 0; i < m->n; i++)
		run_close(m->runs[i].run);
	xtc_free(m->runs);
	free(m->dir);
	xtc_free(m);
}

uint64_t
mf_boundary(const manifest_t *m)
{
	return m->boundary;
}

uint32_t
mf_nruns(const manifest_t *m)
{
	return m->n;
}

/* ---- flush ---- */

/* Write the frozen delta into run `id`: XTC_OK with the run's meta. */
static int
mf_write_run(manifest_t *m, uint64_t id, mf_row_fn rowfn, void *ctx,
    run_meta_t *meta)
{
	run_writer_t *w = NULL;
	uint32_t tid = 0, frame;
	int64_t rid = INT64_MIN;
	run_row_t row;
	char p[1100];
	int rc;

	run_path(m->dir, id, p, sizeof p);
	if ((rc = run_writer_open(m->bm, bm_page_size(m->bm), p, id,
	    MF_LEVEL_N0, &w)) != XTC_OK)
		return rc;
	mf_buggify("rowlog.flush.first_step");
	xtc_rcu_read_lock();
	while (kd_delta_next(m->kd, 1, &tid, &rid, &frame)) {
		memset(&row, 0, sizeof row);
		row.tableid = tid;
		row.rowid = rid;
		if ((rc = rowfn(ctx, frame, &row)) != XTC_OK)
			break;
		row.tableid = tid;
		row.rowid = rid;
		if ((rc = run_writer_add(w, &row)) != XTC_OK)
			break;
		/* The next RID: (tid, rid + 1), or the next table. */
		if (rid == INT64_MAX) {
			if (tid == UINT32_MAX)
				break;
			tid++;
			rid = INT64_MIN;
		} else {
			rid++;
		}
	}
	xtc_rcu_read_unlock();
	if (rc != XTC_OK) {
		run_writer_abort(w);
		return rc;
	}
	return run_writer_finish(w, meta);
}

int
mf_flush_frozen(manifest_t *m, mf_row_fn rowfn, void *ctx)
{
	struct mf_job j;
	run_meta_t meta;
	run_t *r = NULL;
	char p[1100], *text;
	uint64_t id;
	size_t len;
	uint32_t tid = 0, frame;
	int64_t rid = INT64_MIN;
	int empty, rc, rc2;

	if (m == NULL || rowfn == NULL || !kd_has_frozen(m->kd))
		return XTC_E_INVAL;
	if (m->failed)
		return XTC_E_IO;
	xtc_rcu_read_lock();
	empty = !kd_delta_next(m->kd, 1, &tid, &rid, &frame);
	xtc_rcu_read_unlock();
	/* The boundary only rises: a lower one would point recovery at
	 * WAL that retention may already have deleted (a bad boundary_fn).
	 * Refused before anything is written; the delta stays frozen. */
	if (m->frz_boundary < m->boundary)
		return XTC_E_INVAL;
	if (empty) {
		kd_drop_frozen(m->kd);       /* nothing to write */
		xtc_rcu_synchronize();
		return XTC_E_NOTFOUND;
	}
	/* A fresh id, never one sharing a live run's file number; ids of
	 * failed flushes are skipped (a crash may reissue an id whose run
	 * was never installed: no durable state names it). */
	do
		id = m->next++;
	while (mf_file_busy(m, id));
	if ((rc = mf_write_run(m, id, rowfn, ctx, &meta)) != XTC_OK)
		return rc;
	/* Open it (it is checked), then make the manifest name it. */
	run_path(m->dir, id, p, sizeof p);
	if ((rc = run_open(m->bm, p, &meta, &r)) != XTC_OK)
		goto unlink;
	mf_buggify("rowlog.flush.before_install");
	if ((rc = mf_text(m, &meta, m->next, m->frz_boundary, &text,
	    &len)) != XTC_OK)
		goto close;
	memset(&j, 0, sizeof j);
	j.dir = m->dir;
	j.text = text;
	j.len = len;
	j.op = 1;
	rc = mf_job(&j);
	free(text);
	if (rc != XTC_OK && !j.renamed)
		goto close;
	/*
	 * Past the rename the visible MANIFEST names the run and the new
	 * boundary, and it may reach the disk even if the directory fsync
	 * failed: the run must stay, and the in-memory state follows the
	 * new manifest.  An fsync failure cannot be retried (the kernel may
	 * have dropped what it could not write), so the manifest fail-stops
	 * (PostgreSQL's rule): this and every later flush return XTC_E_IO
	 * and the caller fail-stops the log.
	 */
	if (rc != XTC_OK)
		m->failed = 1;
	m->boundary = m->frz_boundary;
	/* Out of memory here leaves the install durable but the run set
	 * stale: fail-stop rather than let a retry write the rows again. */
	if ((rc2 = mf_push(m, &meta, r)) != XTC_OK) {
		run_close(r);
		m->failed = 1;
		return rc2;
	}
	if ((rc2 = mf_publish(m)) != XTC_OK) {
		m->failed = 1;
		return rc2;
	}
	kd_drop_frozen(m->kd);
	/* Synchronize now: libxtc's RCU frees retired objects only in a
	 * synchronize (it parks a fiber while readers drain), and nothing
	 * else in the engine calls it.  The objects retired here are freed
	 * by the next flush's synchronize (or xtc_rcu_fini). */
	xtc_rcu_synchronize();
	return rc;
close:
	run_close(r);
unlink:
	memset(&j, 0, sizeof j);
	snprintf(p, sizeof p, "rl-%" PRIu64 ".run", id);
	j.dir = m->dir;
	j.op = 2;
	j.name = p;
	(void)mf_job(&j);
	return rc;
}

int
mf_flush(manifest_t *m, mf_row_fn rowfn, void *ctx,
    uint64_t (*boundary_fn)(void *ctx), void *bctx)
{
	uint64_t b;
	int rc;

	if (m == NULL || rowfn == NULL || boundary_fn == NULL)
		return XTC_E_INVAL;
	/* Back-pressure first: the flusher is the only freezer, so a frozen
	 * delta seen here is still there at the freeze, and its boundary
	 * must stay.  Then the boundary, then the switch: any time before
	 * the switch is sound (kd_flush_boundary's NB), after it is not. */
	if (kd_has_frozen(m->kd))
		return XTC_E_AGAIN;
	b = boundary_fn(bctx);
	if ((rc = kd_freeze(m->kd, NULL, NULL)) != XTC_OK)
		return rc;
	m->frz_boundary = b;
	return mf_flush_frozen(m, rowfn, ctx);
}
