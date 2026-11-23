/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/05_rexis/bitcask.h -- a Bitcask-style on-disk KV store.
 *
 * Bitcask (Riak, 2010) is a log-structured key/value store
 * optimized for write throughput and fast lookups:
 *
 *   * One append-only data file accumulates every put/delete.
 *   * An in-memory hash index maps key -> (offset, len, timestamp).
 *   * A get is an index lookup + one pread.
 *   * A put is an append + index update.
 *   * A delete is a tombstone append + index removal.  Several
 *     deletes can go in one batch record, which a crash keeps whole
 *     or not at all.
 *   * A flush is one flush record + an emptied index.
 *
 * On crash, the index is rebuilt by scanning the data file at
 * startup.  bitcask_compact (the merge) reclaims the space superseded
 * entries hold; nothing calls it on a schedule.
 *
 * Keys and values are byte strings.  A put may carry meta: a one-byte
 * tag the caller defines and an
 * absolute expiry time in wall-clock nanoseconds.  An entry whose
 * expiry has passed is dropped by recovery and by the merge.
 *
 * Writes go to the OS page cache with pwrite and are not fsync'd, so
 * an acknowledged put survives the process being killed but not a
 * power loss or a kernel crash (bitcask_sync forces it).  The I/O is
 * synchronous on the calling thread.
 *
 * The data file is DIR/bitcask.v2.  A directory written by an older
 * rexis holds DIR/bitcask.data, whose records have a different header:
 * bitcask_open converts it to bitcask.v2 (bitcask.c says how, and how a
 * crash in the middle resumes) and carries on.  An older binary cannot
 * read a converted directory: it finds no bitcask.data and starts
 * empty, leaving bitcask.v2 alone.  It does create an empty
 * bitcask.data; the next bitcask_open removes that, and refuses to open
 * a directory whose bitcask.data holds records next to a bitcask.v2.
 *
 * At open, a torn tail (a record whose header checks out but whose
 * body runs past the end of the file, which is what a crash in the
 * middle of the last write leaves) is cut off.  Any other damage
 * refuses the open, unless it is in the last record and nothing valid
 * follows it: then it is cut like a torn tail.
 */

#ifndef BITCASK_H
#define BITCASK_H

#include <stddef.h>
#include <stdint.h>

typedef struct bitcask bitcask_t;

/* Largest key or value a record holds: rexis's largest bulk string
 * (proto.c), so any value a client can SET can be logged. */
#define BITCASK_MAX_VAL  (512u * 1024u * 1024u)

/* A put's meta.  { 0, 0 } is a plain put. */
typedef struct bitcask_meta {
	uint8_t tag;         /* the caller's; bitcask only stores it */
	int64_t expire_at;   /* wall-clock ns (xtc_clock_real); 0 = never */
} bitcask_meta_t;

typedef struct bitcask_stats {
	uint64_t n_keys;
	uint64_t bytes_used;
	uint64_t bytes_dead;     /* superseded entries waiting for merge */
	uint64_t puts;
	uint64_t gets;
	uint64_t dels;
	uint64_t hits;
	uint64_t misses;
} bitcask_stats_t;

/* Open / create a Bitcask at the given directory.  The directory
 * must already exist; the data file is named "bitcask.v2" inside (an
 * older "bitcask.data" is converted to it, see above). */
int   bitcask_open(const char *dir, bitcask_t **out);

/* Close + flush.  Subsequent opens of the same directory will
 * recover state from the data file. */
void  bitcask_close(bitcask_t *bc);

/* Put.  Returns 0 on success, -1 on I/O error or a key or value over
 * BITCASK_MAX_VAL.  The key and the value may be empty. */
int   bitcask_put(bitcask_t *bc,
                  const void *key, size_t key_len,
                  const void *val, size_t val_len);

/* Put with meta (tag, expiry).  As bitcask_put otherwise. */
int   bitcask_put_meta(bitcask_t *bc,
                       const void *key, size_t key_len,
                       const void *val, size_t val_len,
                       const bitcask_meta_t *m);

/* The meta of `key`'s live entry.  0 on hit, 1 on miss. */
int   bitcask_meta(bitcask_t *bc, const void *key, size_t key_len,
                   bitcask_meta_t *out);

/* Get.  Caller-allocated `val_out` of `val_buf_size` bytes.  On
 * success, *val_len is the actual value size; if val_buf_size is
 * smaller than the value, only the first val_buf_size bytes are
 * copied (callers should size their buffer or call get_size first).
 * Returns 0 on hit, 1 on miss, -1 on I/O error. */
int   bitcask_get(bitcask_t *bc,
                  const void *key, size_t key_len,
                  void *val_out, size_t val_buf_size,
                  size_t *val_len);

/* Lookup the size of the value for `key` without reading it.
 * Returns 0 on hit (size in *val_len), 1 on miss. */
int   bitcask_size(bitcask_t *bc,
                   const void *key, size_t key_len,
                   size_t *val_len);

/* Delete.  Returns 0 if the key existed, 1 if not, -1 on I/O. */
int   bitcask_del(bitcask_t *bc,
                  const void *key, size_t key_len);

/* Delete n keys with one record (a batch of tombstones), so a crash
 * keeps all of the deletes or none.  Returns 0, or -1 on I/O error or
 * a key over BITCASK_MAX_VAL (nothing is deleted then). */
int   bitcask_del_many(bitcask_t *bc, const void *const *keys,
                       const size_t *lens, size_t n);

/* Delete every key: one flush record, after which every earlier
 * record is dead.  Returns 0, or -1 on I/O error. */
int   bitcask_flush(bitcask_t *bc);

/* Iterate all live keys.  Callback receives (key, key_len, user)
 * and returns nonzero to stop early. */
typedef int (*bitcask_iter_fn)(const void *key, size_t key_len, void *user);
int   bitcask_iterate(bitcask_t *bc, bitcask_iter_fn fn, void *user);

/* Force fsync of the data file.  Returns 0 on success. */
int   bitcask_sync(bitcask_t *bc);

/* Compaction / merge: rewrite the data file keeping only the live
 * records, reclaiming the bytes_dead bytes left by overwrites, deletes
 * and flushes.  A live entry keeps its meta; an expired one is dropped.
 * Returns 0 on success.  A failed compaction leaves the
 * handle inconsistent and should be treated as fatal for it.  Holds
 * the write lock for the whole pass. */
int   bitcask_compact(bitcask_t *bc);

/* Stats snapshot. */
void  bitcask_stat(bitcask_t *bc, bitcask_stats_t *out);

/* Test hook: set to N > 0 and the Nth allocation or read of the next
 * damage scan (the search for a valid record after a bad one, at open)
 * fails, as an out-of-memory or an I/O error would.  0 (the default)
 * injects nothing. */
extern int bitcask_scan_fail_at;

#endif
