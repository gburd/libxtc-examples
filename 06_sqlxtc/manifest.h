/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/manifest.h
 *	The row log's MANIFEST (STORAGE_DESIGN 7.2) and the nursery flush
 *	to level n0 (9.2).
 *
 *	MANIFEST is one small text file in the database directory:
 *
 *	    sqlxtc-manifest 1
 *	    next <next run id>
 *	    boundary <flush boundary, a WAL address, hex>
 *	    run <id> <level> <slot> <rows> <header crc>     (one per run)
 *	    crc <CRC32C of every byte before this line, hex>
 *
 *	A change installs a new copy: MANIFEST.tmp written and fsynced,
 *	renamed over MANIFEST, the directory fsynced.  A run file is fsynced
 *	before the manifest that names it and unlinked only after a
 *	manifest that no longer names it is durable, so the durable
 *	manifest always names only complete runs.  At open, run files the
 *	manifest does not name are deleted (7.4 step 1).
 *
 *	The flush writes the frozen delta into a new run at level n0 (in
 *	phase b nothing merges, so n0 only grows; every run is slot A) and
 *	installs it with the new flush boundary.  The row bytes come from
 *	the caller (the row cache owns the frames).
 */

#ifndef SQLXTC_MANIFEST_H
#define SQLXTC_MANIFEST_H

#include <stdint.h>

#include "bufmgr.h"
#include "keydir.h"
#include "run.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MF_LEVEL_N0 16u           /* 2^16-row nursery (9.1) */

typedef struct manifest manifest_t;

/*
 * Open the manifest of directory `dir`, open every run it names through
 * bm into kd's run set, and delete the run files it does not name.  A
 * directory with no MANIFEST and no run file is a new store: an empty
 * manifest (next run 1, boundary 0, no run) is installed at once, so a
 * missing MANIFEST later is always damage.  XTC_E_IO, deleting nothing
 * and naming the problem on stderr: a MANIFEST that cannot be read
 * (anything but "absent"), whose CRC fails, or that names a run that
 * fails run_open, and a missing MANIFEST beside run files.  File work
 * runs under xtc_blocking_run.
 */
int  mf_open(const char *dir, bm_t *bm, keydir_t *kd, manifest_t **out);

/* Close the runs and free.  NB: no keydir reader may be active. */
void mf_close(manifest_t *m);

/* The flush boundary of the durable manifest (a WAL address), and the
 * number of runs. */
uint64_t mf_boundary(const manifest_t *m);
uint32_t mf_nruns(const manifest_t *m);

/*
 * The row bytes of a frozen-delta entry, for the flush: fill *row
 * (tableid and rowid are set; the callee sets commit_ts, self_wa,
 * flags, len and rec, which must stay valid until the next call) and
 * return XTC_OK.
 */
typedef int (*mf_row_fn)(void *ctx, uint32_t frame, run_row_t *row);

/*
 * Flush the nursery: freeze kd's active delta, setting the flush
 * boundary at the switch (*boundary_fn(ctx) is called from kd_freeze's
 * at-freeze hook and returns kd_flush_boundary over the addresses the
 * commits in flight recorded on entry, or wal_end_wa(); see kd_freeze's
 * NB), write the frozen rows into a new
 * run at level MF_LEVEL_N0, fsync it, install a manifest that adds the
 * run and sets the boundary, publish the new run set in kd, and drop
 * the frozen delta.  XTC_E_AGAIN if a frozen delta exists already;
 * XTC_E_NOTFOUND (nothing installed) when the delta was empty.  On a
 * failed write or a failed install before the manifest's rename, the
 * run file is removed, the manifest and the run set are untouched and
 * the delta stays frozen: a later flush retries it (mf_flush_frozen).
 * NB: a failure after the rename (the directory fsync) returns
 * XTC_E_IO with the run installed in memory and kept on disk, and
 * fail-stops the manifest: every later flush returns XTC_E_IO, and the
 * caller must fail-stop the WAL too (wal_fail_stop), since the new
 * MANIFEST may or may not be durable.  On success the frozen delta and the
 * old run set are reclaimed before the return (xtc_rcu_synchronize, so
 * the call waits for keydir readers to leave their read-sides: the
 * flusher must not hold one).  The caller unpins the flushed frames
 * after it returns XTC_OK.
 */
int  mf_flush(manifest_t *m, mf_row_fn row, void *row_ctx,
         uint64_t (*boundary_fn)(void *ctx), void *bctx);

/* Retry writing a frozen delta left by a failed mf_flush, with the
 * boundary the freeze set. */
int  mf_flush_frozen(manifest_t *m, mf_row_fn row, void *row_ctx);

#ifdef __cplusplus
}
#endif

#endif /* SQLXTC_MANIFEST_H */
