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
	uint64_t       frz_boundary;  /* set at the last freeze */
	uint64_t     (*bfn)(void *);  /* the freeze's boundary source */
	void          *bctx;
	struct mf_run *runs;          /* oldest first */
	uint32_t       n, cap;
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
	int         op;               /* 0 read, 1 write, 2 unlink */
	const char *name;             /* 2: the file in dir to unlink */
};

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
	if (!xtc_fs_exists(p))
		return XTC_E_NOTFOUND;
	if ((rc = xtc_fs_open(p, XTC_FS_READ, &fd)) != XTC_OK)
		return rc;
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
 * directory fsynced. */
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
	if (rc == XTC_OK)
		rc = mf_sync_dir(j->dir);
	if (rc != XTC_OK)
		(void)xtc_fs_unlink(tmp);
	return rc;
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

/* The manifest text for runs[0..n) (and one more, `add`, if not NULL),
 * next and boundary.  malloc'd; *len its length. */
static char *
mf_text(const manifest_t *m, const run_meta_t *add, uint64_t next,
    uint64_t boundary, size_t *len)
{
	size_t cap = 128u + 64u * ((size_t)m->n + 1u), at;
	char *t;
	uint32_t i;

	if ((t = malloc(cap)) == NULL)
		return NULL;
	at = (size_t)snprintf(t, cap, "%snext %" PRIu64 "\nboundary %016"
	    PRIx64 "\n", MF_MAGIC, next, boundary);
	for (i = 0; i <= m->n; i++) {
		const run_meta_t *r = i < m->n ? &m->runs[i].meta : add;
		if (r != NULL)
			at += run_meta_enc(r, t + at, cap - at);
	}
	at += (size_t)snprintf(t + at, cap - at, "crc %08x\n",
	    wal_crc32c(0, t, at));
	*len = at;
	return t;
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
	line = strchr(line, '\n') + 1;
	line = strchr(line, '\n') + 1;
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

/* Delete rl-*.run files of `dir` that no run in m names, and a stale
 * MANIFEST.tmp. */
static void
mf_sweep(manifest_t *m)
{
	struct mf_job j;
	xtc_fs_dir_t *d = NULL;
	const char *name;
	char **dead = NULL, **nd;
	uint32_t nn = 0, i, k;
	uint64_t id;
	char tail;

	if (xtc_fs_dir_open(m->dir, &d) != XTC_OK)
		return;
	while (xtc_fs_dir_next(d, &name) == XTC_OK && name != NULL) {
		int named = 0;
		if (strcmp(name, MF_TMP) != 0) {
			if (sscanf(name, "rl-%" SCNu64 ".ru%c", &id, &tail) !=
			    2 || tail != 'n')
				continue;
			for (k = 0; k < m->n && !named; k++)
				named = m->runs[k].meta.runid == id;
			if (named)
				continue;
		}
		if ((nd = realloc(dead, (nn + 1u) * sizeof *nd)) == NULL)
			break;
		dead = nd;
		if ((dead[nn] = strdup(name)) != NULL)
			nn++;
	}
	xtc_fs_dir_close(d);
	for (i = 0; i < nn; i++) {
		memset(&j, 0, sizeof j);
		j.dir = m->dir;
		j.op = 2;
		j.name = dead[i];
		(void)mf_job(&j);
		free(dead[i]);
	}
	free(dead);
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
		rc = XTC_OK;                 /* a new store */
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

/* kd_freeze's at-freeze hook: the boundary, read at the switch. */
static void
mf_at_freeze(void *arg)
{
	manifest_t *m = arg;

	m->frz_boundary = m->bfn(m->bctx);
}

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
	int rc;

	uint32_t tid = 0, frame;
	int64_t rid = INT64_MIN;
	int empty;

	if (m == NULL || rowfn == NULL || !kd_has_frozen(m->kd))
		return XTC_E_INVAL;
	xtc_rcu_read_lock();
	empty = !kd_delta_next(m->kd, 1, &tid, &rid, &frame);
	xtc_rcu_read_unlock();
	if (empty) {
		kd_drop_frozen(m->kd);       /* nothing to write */
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
	if ((text = mf_text(m, &meta, m->next, m->frz_boundary,
	    &j.len)) == NULL) {
		rc = XTC_E_NOMEM;
		goto close;
	}
	j.dir = m->dir;
	j.text = text;
	j.op = 1;
	rc = mf_job(&j);
	free(text);
	if (rc != XTC_OK)
		goto close;
	/* Durable: publish, then the frozen rows are in a run. */
	m->boundary = m->frz_boundary;
	if ((rc = mf_push(m, &meta, r)) != XTC_OK ||
	    (rc = mf_publish(m)) != XTC_OK)
		return rc;                   /* durable; the next open sees it */
	kd_drop_frozen(m->kd);
	return XTC_OK;
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
	int rc;

	if (m == NULL || rowfn == NULL || boundary_fn == NULL)
		return XTC_E_INVAL;
	m->bfn = boundary_fn;
	m->bctx = bctx;
	if ((rc = kd_freeze(m->kd, mf_at_freeze, m)) != XTC_OK)
		return rc;
	return mf_flush_frozen(m, rowfn, ctx);
}
