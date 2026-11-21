/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/run.h
 *	Run files of the row log (STORAGE_DESIGN 3.3).  A run holds the
 *	current versions of a set of rows, sorted by (tableid, rowid), in
 *	pages the buffer manager reads, caches and checks like the page
 *	file's (as file number RUN_FILE(runid), bm_file_attach).  It is
 *	written once, front to back, by run_writer_*, and never changed.
 *
 *	    page 0          header: magic, run id, level, counts, the
 *	                    footer's place, the key range
 *	    pages 1..d      data pages: rows of ONE table each, in RID
 *	                    order; a row too big for a page continues in
 *	                    the pages right after it (and ends its page)
 *	    pages d+1..     footer: a byte stream over the pages' payloads:
 *	                    the table directory, the chunk directory, then
 *	                    the Elias-Fano chunks (ef.h), EF_CHUNK rowids of
 *	                    one table each, with their page maps
 *
 *	Every page has the btnode layout's two fixed slots: the page LSN
 *	at 0 (all ones: no redo ever applies to a run page) and the
 *	checksum at BTNODE_CKSUM_OFF, sealed for page id (RUN_FILE(runid),
 *	page).  Each page also carries its run id, which the reader checks:
 *	file numbers are 16 bits and repeat, run ids never do.
 *
 *	A row record: rowid 8, commit_ts 8, self_wa 8, flags 1, len 2, then
 *	len bytes (the xstore_rec_put encoding).  27 bytes of header.
 */

#ifndef SQLXTC_RUN_H
#define SQLXTC_RUN_H

#include <stddef.h>
#include <stdint.h>

#include "bufmgr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RUN_MAGIC      0x4E555258u   /* "XRUN" */
#define RUN_VERSION    1u
#define RUN_PG_HDR     48u           /* page header, incl. LSN + checksum */
#define RUN_ROW_HDR    27u
#define RUN_F_DELETED  0x01u         /* the row record is a tombstone */
#define RUN_REC_MAX    65535u        /* len is 16 bits */

/* The buffer-manager file number of a run.  Never 0 (the page file). */
#define RUN_FILE(runid) ((uint16_t)(1u + (uint32_t)((runid) % 65535u)))

/* One row as the writer takes it and the reader returns it. */
typedef struct run_row {
	uint32_t       tableid;
	int64_t        rowid;
	uint64_t       commit_ts;
	uint64_t       self_wa;
	uint8_t        flags;
	uint16_t       len;
	const uint8_t *rec;          /* writer: len bytes; reader: see below */
} run_row_t;

/*
 * The manifest's line for a run (STORAGE_DESIGN 7.2): its id, level,
 * slot (0 = A, 1 = B, 2 = C), row count, and the header's CRC, which
 * run_open checks so a manifest never adopts another file under the
 * same name.
 */
typedef struct run_meta {
	uint64_t runid;
	uint32_t level;
	uint32_t slot;
	uint64_t nrows;
	uint32_t hdr_crc;
} run_meta_t;

/* Encode / decode one manifest line ("run <id> <level> <slot> <rows>
 * <crc>\n", decimal and hex).  enc returns the length (< cap) or 0;
 * dec returns 1 on a well-formed line. */
size_t run_meta_enc(const run_meta_t *m, char *buf, size_t cap);
int    run_meta_dec(const char *line, run_meta_t *m);

/* The file name of a run in a directory: <dir>/rl-<runid>.run. */
void run_path(const char *dir, uint64_t runid, char *out, size_t cap);

/* ---- writer ---- */

typedef struct run_writer run_writer_t;

/*
 * Create run file `path` for run `runid` at `level`.  Pages are
 * bm's page size and sealed with bm's checksum (bm_seal_page); bm is
 * not otherwise touched.  The file must not exist.  I/O goes through
 * xtc_aio (a fiber parks), the open and the unlink through
 * xtc_blocking_run.
 */
int run_writer_open(bm_t *bm, uint32_t page_size, const char *path,
    uint64_t runid, uint32_t level, run_writer_t **out);

/*
 * Append a row.  Rows must come in strictly increasing (tableid,
 * rowid) order: XTC_E_INVAL otherwise, or for len > RUN_REC_MAX.
 * XTC_E_IO if a write failed (the writer is then dead: abort it).
 */
int run_writer_add(run_writer_t *w, const run_row_t *row);

/*
 * Write the footer and the header, fsync the file, close it and free
 * the writer.  *meta gets the manifest line (slot 0).  A run of zero
 * rows is XTC_E_INVAL (the caller installs nothing).  On any error the
 * file is removed and the writer freed.
 */
int run_writer_finish(run_writer_t *w, run_meta_t *meta);

/* Drop an unfinished run: close, unlink, free. */
void run_writer_abort(run_writer_t *w);

/* ---- reader ---- */

typedef struct run run_t;

/*
 * Open run `meta->runid` from `path`: attach it to bm as RUN_FILE, read
 * and check the header against meta (id, level, row count, header
 * CRC), and load the table and chunk directories (the resident part:
 * about 40 bytes per table and 16 per EF_CHUNK rows).  Any mismatch or
 * damaged page is XTC_E_IO, and the file is detached again.
 */
int run_open(bm_t *bm, const char *path, const run_meta_t *meta,
    run_t **out);

/* Detach and free.  NB: no reader may still use the run. */
void run_close(run_t *r);

uint64_t run_id(const run_t *r);
uint64_t run_rows(const run_t *r);

/*
 * 1 if the run's table directory admits (tableid, rowid): it has rows of
 * that table and rowid is within their range.  No I/O.
 */
int run_may_hold(const run_t *r, uint32_t tableid, int64_t rowid);

/*
 * Look up a RID: XTC_OK with the row's rank in the run and its data
 * page, XTC_E_NOTFOUND if the run does not hold it, XTC_E_IO on a bad
 * page or chunk.  Reads one chunk (one or two footer pages).
 */
int run_find(run_t *r, uint32_t tableid, int64_t rowid, uint64_t *rank,
    uint32_t *page);

/*
 * Read the row (tableid, rowid) from data page `page` (from run_find):
 * the header fields into *out and up to cap record bytes into buf
 * (out->rec = buf, out->len = the record's full length; a record longer
 * than cap is truncated to cap).  XTC_E_NOTFOUND if the page does not
 * hold the row, XTC_E_IO on a bad page.
 */
int run_read(run_t *r, uint32_t page, uint32_t tableid, int64_t rowid,
    run_row_t *out, uint8_t *buf, size_t cap);

/* run_find then run_read. */
int run_get(run_t *r, uint32_t tableid, int64_t rowid, run_row_t *out,
    uint8_t *buf, size_t cap);

/*
 * Sequential scan in (tableid, rowid) order, for range scans and the
 * merge.  run_iter_next fills *out (rec into buf, as run_read) and
 * returns XTC_OK, XTC_E_NOTFOUND at the end, or XTC_E_IO.
 */
typedef struct run_iter run_iter_t;
int  run_iter_open(run_t *r, run_iter_t **out);
int  run_iter_next(run_iter_t *it, run_row_t *out, uint8_t *buf, size_t cap);
void run_iter_close(run_iter_t *it);

#ifdef __cplusplus
}
#endif

#endif /* SQLXTC_RUN_H */
