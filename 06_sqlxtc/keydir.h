/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/keydir.h
 *	The row log's keydir (STORAGE_DESIGN 5): where is the current
 *	version of RID (tableid, rowid)?  Three parts (5.1):
 *
 *	  resident map  RID -> frame number for every resident row frame:
 *	                a striped open-addressing hash, lock-free readers
 *	                (a per-stripe sequence count), a short writer lock
 *	                per stripe that never parks, and a per-stripe
 *	                change counter for the miss-publish protocol (4.5,
 *	                5.4);
 *	  delta         the nursery: RID -> frame for rows committed since
 *	                the last flush, one ordered map (xtc_cskip) per
 *	                table, an active one taking writes and at most one
 *	                frozen one being flushed (9.2);
 *	  static stage  the runs (run.h), newest first, RCU-published.
 *
 *	A frame number is the caller's (the row cache, phase e); the keydir
 *	stores it and never interprets it.  0 is never a frame.
 *
 *	Readers of the delta and the run set must hold an xtc_rcu read-side
 *	(xtc_rcu_read_lock) across the call and across any use of what it
 *	returns; the resident map needs none.  Writers of the delta are
 *	serialized per table by xtc_cskip; kd_freeze and kd_runs_set are
 *	the flusher's and must not race each other.
 */

#ifndef SQLXTC_KEYDIR_H
#define SQLXTC_KEYDIR_H

#include <stddef.h>
#include <stdint.h>

#include "run.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct keydir keydir_t;

/* Create a keydir whose resident map holds up to `cap` entries (the row
 * cache's frame count; rounded up). */
int  kd_create(uint32_t cap, keydir_t **out);
/* NB: no reader or writer may be active; runs are not closed. */
void kd_destroy(keydir_t *kd);

/* ---- resident map ---- */

/* The frame of a resident RID: 1 and *frame, or 0.  Lock-free. */
int      kd_res_get(keydir_t *kd, uint32_t tid, int64_t rid, uint32_t *frame);

/* The change counter of RID's stripe: sample it before a miss reads
 * the durable copy (4.5). */
uint64_t kd_res_ver(keydir_t *kd, uint32_t tid, int64_t rid);

/* A commit installs RID's current frame: insert or replace, and bump
 * the stripe's change counter.  XTC_E_RESOURCE when the map is full. */
int      kd_res_put(keydir_t *kd, uint32_t tid, int64_t rid, uint32_t frame);

/*
 * A miss publishes the frame it filled, only if RID is still absent and
 * the stripe's change counter still equals `ver` (no commit or drop
 * changed an entry of the stripe since the reader sampled it).  1 if
 * published; 0 if not, with *cur the resident frame if one won the race
 * (0 if none: the counter moved, retry the miss).  Does not bump.
 */
int      kd_res_publish(keydir_t *kd, uint32_t tid, int64_t rid,
             uint32_t frame, uint64_t ver, uint32_t *cur);

/* Remove RID if its frame is `frame` (an eviction: bump = 0; a merge
 * that drops a tombstone: bump = 1).  1 if removed. */
int      kd_res_del(keydir_t *kd, uint32_t tid, int64_t rid, uint32_t frame,
             int bump);

/* Live entries in the resident map. */
uint32_t kd_res_count(keydir_t *kd);

/* ---- delta (the nursery) ---- */

/* A commit adds RID -> frame to the active delta (replacing). */
int  kd_delta_put(keydir_t *kd, uint32_t tid, int64_t rid, uint32_t frame);

/* RID in the active delta, then the frozen one.  RCU read-side held. */
int  kd_delta_get(keydir_t *kd, uint32_t tid, int64_t rid, uint32_t *frame);

/* Rows in the active delta. */
uint64_t kd_delta_rows(keydir_t *kd);

/* Freeze the active delta for a flush and start an empty one.
 * XTC_E_AGAIN while a frozen delta exists (back-pressure, 9.2). */
int  kd_freeze(keydir_t *kd);

/* 1 while a frozen delta exists. */
int  kd_has_frozen(keydir_t *kd);

/*
 * Walk a delta in (tableid, rowid) order from the first RID >= (tid,
 * rid): 1 with the RID and frame, 0 at the end.  frozen = 1 walks the
 * frozen delta (the flush), 0 the active one (a scan).  RCU read-side
 * held.  Each call is a fresh lookup, so it may run across parks.
 */
int  kd_delta_next(keydir_t *kd, int frozen, uint32_t *tid, int64_t *rid,
         uint32_t *frame);

/* Drop the frozen delta once its rows are in an installed run; its
 * memory is retired through RCU. */
void kd_drop_frozen(keydir_t *kd);

/* ---- static stage ---- */

/* Publish the run set, newest first (the array is copied).  The old set
 * is retired through RCU; the run_t objects stay the caller's. */
int  kd_runs_set(keydir_t *kd, run_t *const *runs, uint32_t n);

/* ---- lookup (5.5) ---- */

typedef struct kd_loc {
	int      kind;              /* KD_NONE, KD_FRAME, KD_RUN */
	uint32_t frame;             /* KD_FRAME */
	run_t   *run;               /* KD_RUN */
	uint64_t rank;
	uint32_t page;
} kd_loc_t;

#define KD_NONE  0
#define KD_FRAME 1
#define KD_RUN   2

/*
 * Where RID's current version is: the resident map, else the newest run
 * holding it.  XTC_OK (out->kind KD_NONE if nowhere), or XTC_E_IO from
 * a damaged run.  RCU read-side held (out->run is valid inside it).
 */
int  kd_lookup(keydir_t *kd, uint32_t tid, int64_t rid, kd_loc_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SQLXTC_KEYDIR_H */
