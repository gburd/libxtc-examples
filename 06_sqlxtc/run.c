/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/run.c
 *	Run files: writer and reader.  See run.h for the format.
 *
 *	Header page (page 0), after the 48-byte page header:
 *	    48  u32 version
 *	    52  u32 level
 *	    56  u64 nrows
 *	    64  u32 ntables
 *	    68  u32 nchunks
 *	    72  u32 ndata    (data pages, 1..ndata)
 *	    76  u32 nfooter  (footer pages, ndata+1 ..)
 *	    80  u64 footer bytes
 *	    88  u32 page size
 *	Page header (every page):
 *	    0   u64 page LSN (all ones)
 *	    8   u32 magic RUN_MAGIC
 *	    12  u32 kind ('H' header, 'D' data, 'O' continuation, 'F' footer)
 *	    16  u64 run id
 *	    24  u32 tableid (data and continuation pages)
 *	    28  u16 nslots (data) / payload bytes (continuation)
 *	    30  u16 flags (data: 1 = the last row continues)
 *	    32  u64 rank of the page's first row (data)
 *	    40  checksum slot (BTNODE_CKSUM_OFF)
 *	A data page's slot array (u16 record offsets) starts at 48, the
 *	records are packed from the end down.
 *	Footer stream: ntables x 40-byte table entries
 *	    (tableid 4, nrows 4, first rank 8, first chunk 4, nchunks 4,
 *	     min rowid 8, max rowid 8), then nchunks x 16-byte chunk entries
 *	    (first rowid 8, stream offset 4, length 2, 0 2), then the chunks.
 */

#include "run.h"

#include "btnode.h"
#include "ef.h"
#include "xtc.h"
#include "xtc_aio.h"
#include "xtc_blocking.h"
#include "xtc_fs.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define RUN_K_HDR  'H'
#define RUN_K_DATA 'D'
#define RUN_K_OVF  'O'
#define RUN_K_FTR  'F'
#define RUN_TBL_ENT   40u
#define RUN_CHK_ENT   16u
#define RUN_WBATCH    32u           /* pages per writer pwrite */
#define RUN_CHUNK_MAX (EF_HDR + 8u * 24u + 8u * 504u + 2u * EF_CHUNK)

/* ---- little-endian fields ---- */

static void
p16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void
p32(uint8_t *p, uint32_t v)
{
	p16(p, v);
	p16(p + 2, v >> 16);
}

static void
p64(uint8_t *p, uint64_t v)
{
	p32(p, (uint32_t)v);
	p32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t
g16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t
g32(const uint8_t *p)
{
	return g16(p) | g16(p + 2) << 16;
}

static uint64_t
g64(const uint8_t *p)
{
	return (uint64_t)g32(p) | (uint64_t)g32(p + 4) << 32;
}

/* ---- names and the manifest line ---- */

void
run_path(const char *dir, uint64_t runid, char *out, size_t cap)
{
	snprintf(out, cap, "%s/rl-%" PRIu64 ".run", dir, runid);
}

size_t
run_meta_enc(const run_meta_t *m, char *buf, size_t cap)
{
	int n = snprintf(buf, cap, "run %" PRIu64 " %u %u %" PRIu64 " %08x\n",
	    m->runid, m->level, m->slot, m->nrows, m->hdr_crc);

	return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

int
run_meta_dec(const char *line, run_meta_t *m)
{
	uint64_t id, rows;
	unsigned level, slot, crc;
	int used = 0;

	if (sscanf(line, "run %" SCNu64 " %u %u %" SCNu64 " %8x\n%n", &id,
	    &level, &slot, &rows, &crc, &used) != 5 || used == 0 ||
	    slot > 2 || rows == 0 || id == 0)
		return 0;
	m->runid = id;
	m->level = level;
	m->slot = slot;
	m->nrows = rows;
	m->hdr_crc = crc;
	return 1;
}

/* ---- blocking file work, off the loop ---- */

struct run_fs_job {
	const char *path;
	uint32_t    flags;
	int         fd;
	int         op;              /* 0 open, 1 unlink, 2 close */
};

static int
run_fs_do(void *arg)
{
	struct run_fs_job *j = arg;

	if (j->op == 0)
		return xtc_fs_open(j->path, j->flags, &j->fd);
	if (j->op == 1)
		return xtc_fs_unlink(j->path);
	return xtc_fs_close(j->fd);
}

/* Run one file operation through xtc_blocking_run (inline off a loop).
 * Returns the operation's XTC_* result. */
static int
run_fs(int op, const char *path, uint32_t flags, int *fd)
{
	struct run_fs_job j;
	int rc, res = XTC_E_IO;

	j.path = path;
	j.flags = flags;
	j.fd = fd != NULL ? *fd : -1;
	j.op = op;
	while ((rc = xtc_blocking_run(run_fs_do, &j, &res)) == XTC_E_AGAIN)
		xtc_yield();
	if (rc != XTC_OK)
		return rc;
	if (fd != NULL)
		*fd = j.fd;
	return res;
}

/* ---- writer ---- */

struct run_writer {
	bm_t     *bm;
	uint32_t  ps;
	uint16_t  file;
	uint64_t  runid;
	uint32_t  level;
	int       fd;
	int       dead;
	char      path[1024];

	/* Pages waiting to be written: buf holds nbuf pages, from page
	 * buf_pg on. */
	uint8_t  *buf;
	uint32_t  nbuf, buf_pg;
	uint32_t  next_pg;          /* the next page number to fill */

	/* The open data page (in buf at index cur, or none: cur < 0). */
	int       cur;
	uint32_t  cur_tid;
	uint32_t  cur_n;            /* slots */
	uint32_t  cur_lo;           /* lowest record byte */

	uint64_t  nrows;
	uint32_t  last_tid;
	int64_t   last_rid;

	/* Per-table directory and the chunk under construction. */
	uint8_t  *tbl;              /* RUN_TBL_ENT each */
	uint32_t  ntbl, tbl_cap;
	uint8_t  *chk;              /* RUN_CHK_ENT each */
	uint32_t  nchk, chk_cap;
	uint8_t  *cbytes;           /* the chunks' bytes */
	size_t    clen, ccap;
	int64_t   c_rid[EF_CHUNK];
	uint32_t  c_pg[EF_CHUNK];
	uint32_t  c_n;
};

/* Write the buffered pages and start an empty buffer at next_pg. */
static int
rw_flush(run_writer_t *w)
{
	uint32_t i, len;

	if (w->dead)
		return XTC_E_IO;
	for (i = 0; i < w->nbuf; i++)
		bm_seal_page(w->bm, w->buf + (size_t)i * w->ps,
		    BM_PID_MAKE(w->file, w->buf_pg + i));
	len = w->nbuf * w->ps;
	if (len > 0 && xtc_aio_pwrite(w->fd, w->buf, len,
	    (int64_t)w->buf_pg * w->ps) != (int)len) {
		w->dead = 1;
		return XTC_E_IO;
	}
	w->buf_pg = w->next_pg;
	w->nbuf = 0;
	w->cur = -1;
	return XTC_OK;
}

/* A fresh page of `kind` at next_pg, in the buffer (flushing a full
 * buffer first, and with it any open data page). */
static uint8_t *
rw_page(run_writer_t *w, uint32_t kind, int *rc)
{
	uint8_t *p;

	*rc = XTC_OK;
	if (w->nbuf == RUN_WBATCH && (*rc = rw_flush(w)) != XTC_OK)
		return NULL;
	p = w->buf + (size_t)w->nbuf * w->ps;
	memset(p, 0, w->ps);
	p64(p, ~(uint64_t)0);
	p32(p + 8, RUN_MAGIC);
	p32(p + 12, kind);
	p64(p + 16, w->runid);
	w->nbuf++;
	w->next_pg++;
	return p;
}

/* Grow a byte array to hold `need` bytes. */
static int
rw_grow(uint8_t **a, size_t *cap, size_t need)
{
	size_t n = *cap ? *cap : 4096;
	uint8_t *b;

	if (need <= *cap)
		return XTC_OK;
	while (n < need)
		n *= 2;
	if ((b = xtc_realloc(*a, n)) == NULL)
		return XTC_E_NOMEM;
	*a = b;
	*cap = n;
	return XTC_OK;
}

/* Close the chunk under construction into cbytes and the chunk dir. */
static int
rw_chunk_end(run_writer_t *w)
{
	size_t cap, n, need;
	uint8_t *e;
	int rc;

	if (w->c_n == 0)
		return XTC_OK;
	need = ef_chunk_bytes(w->c_rid, w->c_n, EF_CHUNK);
	if ((rc = rw_grow(&w->cbytes, &w->ccap, w->clen + need)) != XTC_OK)
		return rc;
	n = ef_chunk_build(w->c_rid, w->c_pg, w->c_n, w->cbytes + w->clen,
	    w->ccap - w->clen);
	if (n == 0)
		return XTC_E_INVAL;          /* a page step past EF_MAX_SKIP */
	cap = (size_t)w->chk_cap * RUN_CHK_ENT;
	if ((rc = rw_grow(&w->chk, &cap, (size_t)(w->nchk + 1) *
	    RUN_CHK_ENT)) != XTC_OK)
		return rc;
	w->chk_cap = (uint32_t)(cap / RUN_CHK_ENT);
	e = w->chk + (size_t)w->nchk++ * RUN_CHK_ENT;
	p64(e, (uint64_t)w->c_rid[0]);
	p32(e + 8, (uint32_t)w->clen);        /* offset within the chunks */
	p16(e + 12, (uint32_t)n);
	p16(e + 14, 0);
	w->clen += n;
	w->c_n = 0;
	return XTC_OK;
}

/* Start the table entry for a new tableid. */
static int
rw_table(run_writer_t *w, uint32_t tid, int64_t rid)
{
	size_t cap = (size_t)w->tbl_cap * RUN_TBL_ENT;
	uint8_t *e;
	int rc;

	if ((rc = rw_chunk_end(w)) != XTC_OK)
		return rc;
	if ((rc = rw_grow(&w->tbl, &cap, (size_t)(w->ntbl + 1) *
	    RUN_TBL_ENT)) != XTC_OK)
		return rc;
	w->tbl_cap = (uint32_t)(cap / RUN_TBL_ENT);
	e = w->tbl + (size_t)w->ntbl++ * RUN_TBL_ENT;
	memset(e, 0, RUN_TBL_ENT);
	p32(e, tid);
	p64(e + 8, w->nrows);                 /* first rank */
	p32(e + 16, w->nchk);                 /* first chunk */
	p64(e + 24, (uint64_t)rid);           /* min rowid */
	return XTC_OK;
}

/* Bytes a record of len needs on a data page: header, slot, record. */
static uint32_t
rw_need(uint32_t len)
{
	return 2u + RUN_ROW_HDR + len;
}

/* Put the record's header and as many bytes as fit at the open page's
 * low end; continuation pages take the rest.  Returns XTC_OK. */
static int
rw_put(run_writer_t *w, const run_row_t *r)
{
	uint8_t *pg = w->buf + (size_t)w->cur * w->ps, *rec, *o;
	uint32_t room = w->cur_lo - (RUN_PG_HDR + 2u * (w->cur_n + 1u));
	uint32_t head = RUN_ROW_HDR + r->len, k, done;
	int rc;

	if (head > room)
		head = room;                 /* continues: room >= RUN_ROW_HDR */
	w->cur_lo -= head;
	rec = pg + w->cur_lo;
	p64(rec, (uint64_t)r->rowid);
	p64(rec + 8, r->commit_ts);
	p64(rec + 16, r->self_wa);
	rec[24] = r->flags;
	p16(rec + 25, r->len);
	memcpy(rec + RUN_ROW_HDR, r->rec, head - RUN_ROW_HDR);
	p16(pg + RUN_PG_HDR + 2u * w->cur_n, w->cur_lo);
	w->cur_n++;
	p16(pg + 28, w->cur_n);
	if (head == RUN_ROW_HDR + r->len)
		return XTC_OK;
	/* The rest in continuation pages; the data page is full now. */
	p16(pg + 30, 1);
	w->cur = -1;
	for (done = head - RUN_ROW_HDR; done < r->len; done += k) {
		k = r->len - done;
		if (k > w->ps - RUN_PG_HDR)
			k = w->ps - RUN_PG_HDR;
		if ((o = rw_page(w, RUN_K_OVF, &rc)) == NULL)
			return rc;
		p32(o + 24, r->tableid);
		p16(o + 28, k);
		memcpy(o + RUN_PG_HDR, r->rec + done, k);
	}
	return XTC_OK;
}

int
run_writer_open(bm_t *bm, uint32_t page_size, const char *path,
    uint64_t runid, uint32_t level, run_writer_t **out)
{
	run_writer_t *w;
	int rc;

	if (path == NULL || out == NULL || runid == 0 || page_size < 512 ||
	    page_size > 65536 || strlen(path) >= sizeof w->path)
		return XTC_E_INVAL;
	if ((w = xtc_calloc(1, sizeof *w)) == NULL)
		return XTC_E_NOMEM;
	w->bm = bm;
	w->ps = page_size;
	w->file = RUN_FILE(runid);
	w->runid = runid;
	w->level = level;
	w->cur = -1;
	w->fd = -1;
	snprintf(w->path, sizeof w->path, "%s", path);
	if ((w->buf = xtc_aligned_alloc(4096, (size_t)RUN_WBATCH *
	    page_size)) == NULL) {
		xtc_free(w);
		return XTC_E_NOMEM;
	}
	rc = run_fs(0, path, XTC_FS_READ | XTC_FS_WRITE | XTC_FS_CREATE |
	    XTC_FS_EXCL, &w->fd);
	if (rc != XTC_OK) {
		xtc_aligned_free(w->buf);
		xtc_free(w);
		return rc;
	}
	/* Page 0 (the header) is written last; data starts at page 1. */
	w->next_pg = w->buf_pg = 1;
	*out = w;
	return XTC_OK;
}

int
run_writer_add(run_writer_t *w, const run_row_t *r)
{
	uint8_t *pg;
	int rc;

	if (w == NULL || r == NULL || (r->len > 0 && r->rec == NULL) ||
	    (w->nrows > 0 && (r->tableid < w->last_tid ||
	    (r->tableid == w->last_tid && r->rowid <= w->last_rid))))
		return XTC_E_INVAL;
	if (w->dead)
		return XTC_E_IO;
	/* A new table starts a new table entry, a new chunk and a new page:
	 * a data page holds rows of one table. */
	if (w->nrows == 0 || r->tableid != w->last_tid) {
		if ((rc = rw_table(w, r->tableid, r->rowid)) != XTC_OK)
			return rc;
		w->cur = -1;
	}
	if (w->c_n == EF_CHUNK && (rc = rw_chunk_end(w)) != XTC_OK)
		return rc;
	/* A page with no room for this row's header and a byte (or its
	 * whole record when it is the page's first) is closed. */
	if (w->cur >= 0 && w->cur_lo < RUN_PG_HDR + 2u * (w->cur_n + 1u) +
	    (rw_need(r->len) - 2u))
		w->cur = -1;
	if (w->cur < 0) {
		if ((pg = rw_page(w, RUN_K_DATA, &rc)) == NULL)
			return rc;
		w->cur = (int)w->nbuf - 1;
		w->cur_tid = r->tableid;
		w->cur_n = 0;
		w->cur_lo = w->ps;
		p32(pg + 24, r->tableid);
		p64(pg + 32, w->nrows);
	}
	w->c_rid[w->c_n] = r->rowid;
	w->c_pg[w->c_n] = w->buf_pg + (uint32_t)w->cur;
	w->c_n++;
	if ((rc = rw_put(w, r)) != XTC_OK)
		return rc;
	w->nrows++;
	w->last_tid = r->tableid;
	w->last_rid = r->rowid;
	/* The table entry's row count and max rowid. */
	pg = w->tbl + (size_t)(w->ntbl - 1u) * RUN_TBL_ENT;
	p32(pg + 4, g32(pg + 4) + 1u);
	p64(pg + 32, (uint64_t)r->rowid);
	return XTC_OK;
}

/* Append n bytes to the footer stream (pages of kind 'F'). */
static int
rw_ftr_put(run_writer_t *w, uint32_t *at, uint8_t **pg, const void *src,
    size_t n)
{
	const uint8_t *s = src;
	uint32_t pay = w->ps - RUN_PG_HDR, k;
	int rc;

	while (n > 0) {
		if (*pg == NULL || *at == pay) {
			if ((*pg = rw_page(w, RUN_K_FTR, &rc)) == NULL)
				return rc;
			*at = 0;
		}
		k = pay - *at < n ? pay - *at : (uint32_t)n;
		memcpy(*pg + RUN_PG_HDR + *at, s, k);
		*at += k;
		s += k;
		n -= k;
	}
	return XTC_OK;
}

int
run_writer_finish(run_writer_t *w, run_meta_t *meta)
{
	uint32_t ndata, at = 0, i, base;
	uint8_t *pg = NULL, hdr[RUN_CHK_ENT];
	uint64_t fbytes;
	int rc;

	if (w == NULL || meta == NULL)
		return XTC_E_INVAL;
	if (w->nrows == 0) {
		run_writer_abort(w);
		return XTC_E_INVAL;
	}
	if ((rc = rw_chunk_end(w)) != XTC_OK)
		goto fail;
	/* Each table's chunk count, now that all are closed. */
	for (i = 0; i < w->ntbl; i++) {
		uint8_t *e = w->tbl + (size_t)i * RUN_TBL_ENT;
		uint32_t next = i + 1u < w->ntbl ?
		    g32(e + RUN_TBL_ENT + 16) : w->nchk;
		p32(e + 20, next - g32(e + 16));
	}
	/* The footer: its own pages, after the last data page. */
	w->cur = -1;
	ndata = w->next_pg - 1u;
	base = w->ntbl * RUN_TBL_ENT + w->nchk * RUN_CHK_ENT;
	if ((rc = rw_ftr_put(w, &at, &pg, w->tbl,
	    (size_t)w->ntbl * RUN_TBL_ENT)) != XTC_OK)
		goto fail;
	for (i = 0; i < w->nchk; i++) {
		memcpy(hdr, w->chk + (size_t)i * RUN_CHK_ENT, RUN_CHK_ENT);
		p32(hdr + 8, g32(hdr + 8) + base);   /* stream offset */
		if ((rc = rw_ftr_put(w, &at, &pg, hdr, RUN_CHK_ENT)) != XTC_OK)
			goto fail;
	}
	if ((rc = rw_ftr_put(w, &at, &pg, w->cbytes, w->clen)) != XTC_OK)
		goto fail;
	fbytes = (uint64_t)base + w->clen;
	if ((rc = rw_flush(w)) != XTC_OK)
		goto fail;
	/* The header, written once everything it names is in the file. */
	w->buf_pg = 0;
	w->next_pg = 0;
	if ((pg = rw_page(w, RUN_K_HDR, &rc)) == NULL)
		goto fail;
	p32(pg + 48, RUN_VERSION);
	p32(pg + 52, w->level);
	p64(pg + 56, w->nrows);
	p32(pg + 64, w->ntbl);
	p32(pg + 68, w->nchk);
	p32(pg + 72, ndata);
	p32(pg + 76, (uint32_t)((fbytes + (w->ps - RUN_PG_HDR) - 1u) /
	    (w->ps - RUN_PG_HDR)));
	p64(pg + 80, fbytes);
	p32(pg + 88, w->ps);
	if ((rc = rw_flush(w)) != XTC_OK)
		goto fail;
	if (xtc_aio_fsync(w->fd) != 0) {
		rc = XTC_E_IO;
		goto fail;
	}
	memset(meta, 0, sizeof *meta);
	meta->runid = w->runid;
	meta->level = w->level;
	meta->nrows = w->nrows;
	meta->hdr_crc = g32(w->buf + BTNODE_CKSUM_OFF);
	(void)run_fs(2, NULL, 0, &w->fd);
	w->fd = -1;
	run_writer_abort(w);             /* frees; the file stays */
	return XTC_OK;
fail:
	run_writer_abort(w);
	w = NULL;
	return rc;
}

void
run_writer_abort(run_writer_t *w)
{
	if (w == NULL)
		return;
	if (w->fd >= 0) {
		(void)run_fs(2, NULL, 0, &w->fd);
		(void)run_fs(1, w->path, 0, NULL);
	}
	xtc_aligned_free(w->buf);
	xtc_free(w->tbl);
	xtc_free(w->chk);
	xtc_free(w->cbytes);
	xtc_free(w);
}

/* ---- reader ---- */

struct run {
	bm_t     *bm;
	uint16_t  file;
	uint64_t  runid;
	uint32_t  ps;
	uint64_t  nrows;
	uint32_t  ntbl, nchk, ndata, ftr0;
	uint64_t  fbytes;
	uint8_t  *tbl;               /* ntbl x RUN_TBL_ENT */
	uint8_t  *chk;               /* nchk x RUN_CHK_ENT */
};

struct run_iter {
	run_t    *r;
	uint32_t  page;              /* the data page being read */
	uint32_t  slot;
};

uint64_t
run_id(const run_t *r)
{
	return r->runid;
}

uint64_t
run_rows(const run_t *r)
{
	return r->nrows;
}

/* Fix page pg and check it is a page of this run, of kind `kind` (0:
 * any kind, returned in *got if not NULL). */
static int
rr_fix_k(run_t *r, uint32_t pg, uint32_t kind, bm_frame_t **f,
    uint32_t *got)
{
	const uint8_t *p;
	int rc;

	rc = bm_fix_pid(r->bm, BM_PID_MAKE(r->file, pg), f);
	if (rc != XTC_OK)
		return rc == XTC_E_INVAL ? XTC_E_INVAL : XTC_E_IO;
	p = bm_page(*f);
	if (g32(p + 8) != RUN_MAGIC || g64(p + 16) != r->runid ||
	    (kind != 0 && g32(p + 12) != kind)) {
		bm_unfix(r->bm, *f, 0);
		return XTC_E_IO;
	}
	if (got != NULL)
		*got = g32(p + 12);
	return XTC_OK;
}

static int
rr_fix(run_t *r, uint32_t pg, uint32_t kind, bm_frame_t **f)
{
	return rr_fix_k(r, pg, kind, f, NULL);
}

/* Copy footer stream bytes [off, off + n) into dst. */
static int
rr_ftr(run_t *r, uint64_t off, void *dst, size_t n)
{
	uint32_t pay = r->ps - RUN_PG_HDR, k, in;
	uint8_t *d = dst;
	bm_frame_t *f;
	int rc;

	if (off + n > r->fbytes)
		return XTC_E_IO;
	while (n > 0) {
		in = (uint32_t)(off % pay);
		k = pay - in < n ? pay - in : (uint32_t)n;
		if ((rc = rr_fix(r, r->ftr0 + (uint32_t)(off / pay), RUN_K_FTR,
		    &f)) != XTC_OK)
			return rc;
		memcpy(d, (const uint8_t *)bm_page(f) + RUN_PG_HDR + in, k);
		bm_unfix(r->bm, f, 0);
		d += k;
		off += k;
		n -= k;
	}
	return XTC_OK;
}

/* Check the directories just loaded: tables in order, chunks inside
 * the stream, counts adding up. */
static int
rr_dirs_ok(const run_t *r)
{
	uint64_t rows = 0, base = (uint64_t)r->ntbl * RUN_TBL_ENT +
	    (uint64_t)r->nchk * RUN_CHK_ENT;
	uint32_t i, chunks = 0;

	for (i = 0; i < r->ntbl; i++) {
		const uint8_t *e = r->tbl + (size_t)i * RUN_TBL_ENT;
		uint32_t n = g32(e + 4), nc = g32(e + 20);
		if (n == 0 || g64(e + 8) != rows || g32(e + 16) != chunks ||
		    nc != (n + EF_CHUNK - 1u) / EF_CHUNK ||
		    (i > 0 && g32(e) <= g32(e - RUN_TBL_ENT)) ||
		    (int64_t)g64(e + 24) > (int64_t)g64(e + 32))
			return 0;
		rows += n;
		chunks += nc;
	}
	if (rows != r->nrows || chunks != r->nchk)
		return 0;
	for (i = 0; i < r->nchk; i++) {
		const uint8_t *c = r->chk + (size_t)i * RUN_CHK_ENT;
		if (g32(c + 8) < base || g32(c + 8) + g16(c + 12) > r->fbytes)
			return 0;
	}
	return 1;
}

int
run_open(bm_t *bm, const char *path, const run_meta_t *meta, run_t **out)
{
	const uint8_t *h;
	bm_frame_t *f;
	run_t *r;
	int rc;

	if (bm == NULL || path == NULL || meta == NULL || out == NULL)
		return XTC_E_INVAL;
	if ((r = xtc_calloc(1, sizeof *r)) == NULL)
		return XTC_E_NOMEM;
	r->bm = bm;
	r->file = RUN_FILE(meta->runid);
	r->runid = meta->runid;
	if ((rc = bm_file_attach(bm, r->file, path)) != XTC_OK) {
		xtc_free(r);
		return rc;
	}
	/* The header: it must be the run the manifest names. */
	if ((rc = rr_fix(r, 0, RUN_K_HDR, &f)) != XTC_OK)
		goto fail;
	h = bm_page(f);
	r->ps = g32(h + 88);
	r->nrows = g64(h + 56);
	r->ntbl = g32(h + 64);
	r->nchk = g32(h + 68);
	r->ndata = g32(h + 72);
	r->ftr0 = r->ndata + 1u;
	r->fbytes = g64(h + 80);
	rc = g32(h + 48) == RUN_VERSION && g32(h + 52) == meta->level &&
	    r->nrows == meta->nrows &&
	    g32(h + BTNODE_CKSUM_OFF) == meta->hdr_crc && r->ntbl > 0 &&
	    r->nchk > 0 && r->ps > RUN_PG_HDR + 2u + RUN_ROW_HDR &&
	    r->ndata > 0 && (uint64_t)g32(h + 76) * (r->ps - RUN_PG_HDR) >=
	    r->fbytes ? XTC_OK : XTC_E_IO;
	bm_unfix(bm, f, 0);
	if (rc != XTC_OK)
		goto fail;
	/* The resident part: the table and chunk directories. */
	if ((r->tbl = xtc_malloc((size_t)r->ntbl * RUN_TBL_ENT)) == NULL ||
	    (r->chk = xtc_malloc((size_t)r->nchk * RUN_CHK_ENT)) == NULL) {
		rc = XTC_E_NOMEM;
		goto fail;
	}
	if ((rc = rr_ftr(r, 0, r->tbl, (size_t)r->ntbl * RUN_TBL_ENT)) !=
	    XTC_OK ||
	    (rc = rr_ftr(r, (uint64_t)r->ntbl * RUN_TBL_ENT, r->chk,
	    (size_t)r->nchk * RUN_CHK_ENT)) != XTC_OK)
		goto fail;
	if (!rr_dirs_ok(r)) {
		rc = XTC_E_IO;
		goto fail;
	}
	*out = r;
	return XTC_OK;
fail:
	(void)bm_file_detach(bm, r->file);
	xtc_free(r->tbl);
	xtc_free(r->chk);
	xtc_free(r);
	return rc == XTC_E_NOMEM ? rc : XTC_E_IO;
}

void
run_close(run_t *r)
{
	if (r == NULL)
		return;
	(void)bm_file_detach(r->bm, r->file);
	xtc_free(r->tbl);
	xtc_free(r->chk);
	xtc_free(r);
}

/* The table entry for tableid, or NULL. */
static const uint8_t *
rr_table(const run_t *r, uint32_t tid)
{
	uint32_t lo = 0, hi = r->ntbl;

	while (lo < hi) {
		uint32_t m = lo + (hi - lo) / 2;
		uint32_t t = g32(r->tbl + (size_t)m * RUN_TBL_ENT);
		if (t < tid)
			lo = m + 1;
		else if (t > tid)
			hi = m;
		else
			return r->tbl + (size_t)m * RUN_TBL_ENT;
	}
	return NULL;
}

int
run_may_hold(const run_t *r, uint32_t tableid, int64_t rowid)
{
	const uint8_t *e = rr_table(r, tableid);

	return e != NULL && rowid >= (int64_t)g64(e + 24) &&
	    rowid <= (int64_t)g64(e + 32);
}

int
run_find(run_t *r, uint32_t tableid, int64_t rowid, uint64_t *rank,
    uint32_t *page)
{
	uint8_t chunk[RUN_CHUNK_MAX];
	const uint8_t *e = rr_table(r, tableid), *c;
	uint32_t lo, hi, c0, nc, len, k, pg;
	int rc;

	if (e == NULL || rowid < (int64_t)g64(e + 24) ||
	    rowid > (int64_t)g64(e + 32))
		return XTC_E_NOTFOUND;
	/* The last chunk whose first rowid is <= rowid. */
	c0 = g32(e + 16);
	nc = g32(e + 20);
	lo = 0;
	hi = nc;
	while (hi - lo > 1) {
		uint32_t m = lo + (hi - lo) / 2;
		if ((int64_t)g64(r->chk + (size_t)(c0 + m) * RUN_CHK_ENT) <=
		    rowid)
			lo = m;
		else
			hi = m;
	}
	c = r->chk + (size_t)(c0 + lo) * RUN_CHK_ENT;
	len = g16(c + 12);
	if (len > sizeof chunk)
		return XTC_E_IO;
	if ((rc = rr_ftr(r, g32(c + 8), chunk, len)) != XTC_OK)
		return rc;
	if (!ef_chunk_valid(chunk, len) ||
	    ef_chunk_first(chunk) != (int64_t)g64(c))
		return XTC_E_IO;
	if (!ef_chunk_find(chunk, rowid, &k, &pg))
		return XTC_E_NOTFOUND;
	if (pg == 0 || pg > r->ndata)
		return XTC_E_IO;
	if (rank != NULL)
		*rank = g64(e + 8) + (uint64_t)lo * EF_CHUNK + k;
	if (page != NULL)
		*page = pg;
	return XTC_OK;
}

/* Decode the row at slot i of a fixed data page into *out, copying up
 * to cap record bytes (reading its continuation pages if needed).
 * The page stays fixed; the caller unfixes it. */
static int
rr_row(run_t *r, uint32_t pgno, const uint8_t *p, uint32_t i, run_row_t *out,
    uint8_t *buf, size_t cap)
{
	uint32_t n = g16(p + 28), off, len, have, k, at;
	const uint8_t *rec;
	bm_frame_t *f;
	int rc;

	off = g16(p + RUN_PG_HDR + 2u * i);
	if (i >= n || off < RUN_PG_HDR + 2u * n || off + RUN_ROW_HDR > r->ps)
		return XTC_E_IO;
	rec = p + off;
	len = g16(rec + 25);
	/* Only the page's last row may continue; it ends at the page end
	 * (or at the next row's start). */
	have = (i == 0 ? r->ps : g16(p + RUN_PG_HDR + 2u * (i - 1u))) - off -
	    RUN_ROW_HDR;
	if (have > len)
		have = len;
	if (have < len && !(i + 1u == n && (g16(p + 30) & 1u)))
		return XTC_E_IO;
	out->tableid = g32(p + 24);
	out->rowid = (int64_t)g64(rec);
	out->commit_ts = g64(rec + 8);
	out->self_wa = g64(rec + 16);
	out->flags = rec[24];
	out->len = (uint16_t)len;
	out->rec = buf;
	k = have < cap ? have : (uint32_t)cap;
	if (k > 0)
		memcpy(buf, rec + RUN_ROW_HDR, k);
	/* The rest from the continuation pages after this one. */
	for (at = have, pgno++; at < len && at < cap; pgno++) {
		uint32_t m;
		if ((rc = rr_fix(r, pgno, RUN_K_OVF, &f)) != XTC_OK)
			return rc;
		m = g16((const uint8_t *)bm_page(f) + 28);
		if (m == 0 || m > r->ps - RUN_PG_HDR || at + m > len) {
			bm_unfix(r->bm, f, 0);
			return XTC_E_IO;
		}
		k = m < cap - at ? m : (uint32_t)(cap - at);
		memcpy(buf + at, (const uint8_t *)bm_page(f) + RUN_PG_HDR, k);
		bm_unfix(r->bm, f, 0);
		at += m;
	}
	return XTC_OK;
}

int
run_read(run_t *r, uint32_t page, uint32_t tableid, int64_t rowid,
    run_row_t *out, uint8_t *buf, size_t cap)
{
	const uint8_t *p;
	bm_frame_t *f;
	uint32_t lo, hi, n;
	int rc;

	if (page == 0 || page > r->ndata)
		return XTC_E_INVAL;
	if ((rc = rr_fix(r, page, RUN_K_DATA, &f)) != XTC_OK)
		return rc;
	p = bm_page(f);
	n = g16(p + 28);
	if (n == 0 || RUN_PG_HDR + 2u * n > r->ps) {
		bm_unfix(r->bm, f, 0);
		return XTC_E_IO;
	}
	if (g32(p + 24) != tableid) {
		bm_unfix(r->bm, f, 0);
		return XTC_E_NOTFOUND;
	}
	/* The slots are in rowid order. */
	lo = 0;
	hi = n;
	rc = XTC_E_NOTFOUND;
	while (lo < hi) {
		uint32_t m = lo + (hi - lo) / 2, off;
		int64_t v;
		off = g16(p + RUN_PG_HDR + 2u * m);
		if (off + 8u > r->ps) {
			rc = XTC_E_IO;
			break;
		}
		v = (int64_t)g64(p + off);
		if (v < rowid) {
			lo = m + 1;
		} else if (v > rowid) {
			hi = m;
		} else {
			rc = rr_row(r, page, p, m, out, buf, cap);
			break;
		}
	}
	bm_unfix(r->bm, f, 0);
	return rc;
}

int
run_get(run_t *r, uint32_t tableid, int64_t rowid, run_row_t *out,
    uint8_t *buf, size_t cap)
{
	uint32_t page;
	int rc;

	if ((rc = run_find(r, tableid, rowid, NULL, &page)) != XTC_OK)
		return rc;
	return run_read(r, page, tableid, rowid, out, buf, cap);
}

int
run_iter_open(run_t *r, run_iter_t **out)
{
	run_iter_t *it;

	if (r == NULL || out == NULL)
		return XTC_E_INVAL;
	if ((it = xtc_calloc(1, sizeof *it)) == NULL)
		return XTC_E_NOMEM;
	it->r = r;
	it->page = 1;
	*out = it;
	return XTC_OK;
}

int
run_iter_next(run_iter_t *it, run_row_t *out, uint8_t *buf, size_t cap)
{
	run_t *r = it->r;
	const uint8_t *p;
	bm_frame_t *f;
	uint32_t n, kind;
	int rc;

	for (;;) {
		if (it->page > r->ndata)
			return XTC_E_NOTFOUND;
		if ((rc = rr_fix_k(r, it->page, 0, &f, &kind)) != XTC_OK)
			return XTC_E_IO;
		p = bm_page(f);
		n = kind == RUN_K_DATA ? g16(p + 28) : 0;   /* skip 'O' pages */
		if (kind != RUN_K_DATA && kind != RUN_K_OVF) {
			bm_unfix(r->bm, f, 0);
			return XTC_E_IO;
		}
		if (it->slot < n) {
			rc = rr_row(r, it->page, p, it->slot, out, buf, cap);
			bm_unfix(r->bm, f, 0);
			it->slot++;
			return rc;
		}
		bm_unfix(r->bm, f, 0);
		it->page++;
		it->slot = 0;
	}
}

void
run_iter_close(run_iter_t *it)
{
	xtc_free(it);
}
