/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/05_rexis/bitcask.c -- Bitcask-style on-disk KV.
 *
 * On-disk record format (network byte order):
 *
 *   uint32_t  hcrc;       // CRC32 of (timestamp, key_len, val_len)
 *   uint32_t  crc;        // CRC32 of (timestamp .. value)
 *   uint64_t  timestamp;  // monotonic ns of write
 *   uint32_t  key_len;
 *   uint32_t  val_len;    // the record kind, see below
 *   uint8_t   key[key_len];
 *   uint8_t   meta[9];    // v2 put only: tag, then expire_at (u64)
 *   uint8_t   val[len];   // puts only
 *
 * The header CRC is checked before the lengths are believed, so a
 * damaged length is a bad record, never a record that seems to run
 * past the end of the file.  Both CRCs start from SEED_TOP, except in
 * the records inside a batch, which start from SEED_INNER: a batch's
 * inner records never pass for records of the log.
 *
 * val_len says which kind of record it is:
 *
 *   0 .. BITCASK_MAX_VAL     plain put: val_len value bytes, no meta
 *                            (a put with tag 0 and no expiry).
 *   REC_V2 | len             put with meta: meta, then len value bytes.
 *   REC_FLUSH (0xFFFFFFFE)   flush: key_len 0, no body.  Every
 *                            earlier record is dead.
 *   REC_BATCH (0xFFFFFFFD)   batch: key_len is the length of the
 *                            body, which is whole records of the
 *                            other kinds, back to back.  The CRC
 *                            covers the body, so recovery applies a
 *                            batch whole or cuts it off whole.
 *   REC_TOMBSTONE (~0)       delete: key only.
 *
 * The log of an older rexis (bitcask.data) has 20-byte headers with no
 * header CRC: crc, timestamp, key_len, val_len, and only plain puts and
 * tombstones.  It is converted, never mixed into bitcask.v2: a scan
 * could not tell an old header from a damaged new one.  bitcask_open
 * renames it to bitcask.v1, reads it with the old header, writes its
 * live keys to bitcask.v2 through the merge (a temp file renamed into
 * place, so bitcask.v2 exists only whole), and removes bitcask.v1.
 * An older binary never opens bitcask.v2 (see bitcask.h).
 *
 * Recovery on open: scan the file from start, rebuild the in-memory
 * hash index by replaying every record (the latest entry per key
 * wins; tombstones erase; a flush erases all; a put whose expiry has
 * passed erases its key).
 *
 * Limitations:
 *   * single data file; no rotation.
 *   * one writer at a time (no concurrent put).  Multiple readers
 *     are fine.
 *   * value size capped at BITCASK_MAX_VAL to limit recovery cost.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "bitcask.h"
#include "lhash.h"

#include "xtc.h"     /* public clock + alloc API */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

#define MAX_VAL_LEN  BITCASK_MAX_VAL

/* ---- CRC32 (IEEE 802.3) -------------------------------------- */

static uint32_t __crc_table[256];
static int      __crc_inited;

static void
crc_init(void)
{
	uint32_t i, j, c;
	if (__crc_inited) return;
	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++)
			c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
		__crc_table[i] = c;
	}
	__crc_inited = 1;
}

static uint32_t
crc32(const void *data, size_t len, uint32_t init)
{
	const uint8_t *p = data;
	uint32_t c = init ^ 0xffffffffu;
	size_t i;
	if (!__crc_inited) crc_init();
	for (i = 0; i < len; i++)
		c = __crc_table[(c ^ p[i]) & 0xffu] ^ (c >> 8);
	return c ^ 0xffffffffu;
}

/* ---- bytewise NBO helpers ------------------------------------ */

static void put_u32(uint8_t *p, uint32_t v) {
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void put_u64(uint8_t *p, uint64_t v) {
	put_u32(p, (uint32_t)(v >> 32));
	put_u32(p + 4, (uint32_t)v);
}
static uint32_t get_u32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static uint64_t get_u64(const uint8_t *p) {
	return ((uint64_t)get_u32(p) << 32) | (uint64_t)get_u32(p + 4);
}

#define HDR_SIZE  (4 /*hcrc*/ + 4 /*crc*/ + 8 /*ts*/ + 4 /*kl*/ + 4 /*vl*/)
#define HDR_V1    (4 /*crc*/ + 8 /*ts*/ + 4 /*kl*/ + 4 /*vl*/)
#define SEED_TOP   0x72780201u      /* CRC seed of a record of the log */
#define SEED_INNER 0x72780202u      /* CRC seed of a record in a batch */
#define META_SIZE (1 /*tag*/ + 8 /*expire_at*/)
#define TOMBSTONE 0xffffffffu
#define REC_FLUSH 0xfffffffeu
#define REC_BATCH 0xfffffffdu
#define REC_V2    0x80000000u
#define DATA_FILE "bitcask.v2"
#define V1_FILE   "bitcask.v1"      /* an older log being converted */
#define TMP_FILE  "bitcask.compact" /* the merge's output, renamed in */

/*
 * Fill a record header's timestamp, key_len and val_len, and its header
 * CRC (from `seed`).  Returns the header CRC: the caller runs the body
 * CRC on from it over the body, and stores the result at hdr + 4.
 */
static uint32_t
__hdr_seal(uint8_t *hdr, uint64_t ts, uint32_t kl, uint32_t vl,
    uint32_t seed)
{
	uint32_t hcrc;

	put_u64(hdr + 8, ts);
	put_u32(hdr + 16, kl);
	put_u32(hdr + 20, vl);
	hcrc = crc32(hdr + 8, HDR_SIZE - 8, seed);
	put_u32(hdr, hcrc);
	return hcrc;
}

/* Whether a header's CRC matches its fields, from `seed`. */
static int
__hdr_ok(const uint8_t *hdr, uint32_t seed)
{
	return get_u32(hdr) == crc32(hdr + 8, HDR_SIZE - 8, seed);
}

static int64_t
now_ns(void)
{
	int64_t ns = 0;
	/* Portable monotonic nanoseconds: clock_gettime(CLOCK_MONOTONIC)
	 * on POSIX, mach_continuous_time on macOS, QueryPerformanceCounter
	 * on Windows -- libxtc smooths the platform difference. */
	ns = xtc_clock_mono();
	return ns;
}

/* The clock expiry times are in.  It must be the wall clock: an expiry
 * has to mean the same instant after a restart or a reboot, and the
 * monotonic clock restarts with the host.  The store is never run under
 * the simulator, so the unvirtualized clock is fine here. */
static int64_t
wall_ns(void)
{
	return xtc_clock_real();
}

/* ---- in-memory index ----------------------------------------- */

/* Starting size; the index grows (lhash.h) with the key count.  It
 * used to be a FIXED 4,096-bucket chain table: at 4M keys every put
 * walked a ~1,000-entry chain to find the old record and a 5M-key load
 * fell from 254k to 7.6k puts/s. */
#define IDX_BUCKETS 4096

struct idx_entry {
	lh_node_t           node;      /* chain link + hash; first member */
	uint8_t            *key;       /* malloc'd copy */
	size_t              key_len;
	uint64_t            offset;    /* file offset of value */
	uint32_t            val_len;   /* on-disk length */
	uint64_t            timestamp;
	int64_t             expire_at; /* wall ns; 0 = never */
	uint8_t             tag;       /* caller's tag; 0 = plain */
};

/* Bytes the entry's record takes in the file. */
static uint64_t
__rec_size(const struct idx_entry *e)
{
	int v2 = e->tag != 0 || e->expire_at != 0;
	return (uint64_t)HDR_SIZE + e->key_len + (v2 ? META_SIZE : 0) +
	    e->val_len;
}

static uint64_t
__hash_key(const void *key, size_t len)
{
	const uint8_t *p = key;
	uint64_t h = 14695981039346656037ULL;
	size_t i;
	for (i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ULL; }
	return h;
}

struct bitcask {
	pthread_mutex_t      lock;
	int                  fd;
	char                 dir[480];
	uint32_t             hdr_size;  /* HDR_SIZE; HDR_V1 converting */
	uint64_t             write_offset;
	lh_t                 idx;
	uint64_t             n_keys;
	uint64_t             bytes_used;
	uint64_t             bytes_dead;
	uint64_t             puts, gets, dels, hits, misses;
};

static void *
__idx_alloc(void *ctx, size_t sz)
{
	(void)ctx;
	return calloc(1, sz);
}

static void
__idx_mfree(void *ctx, void *p, size_t sz)
{
	(void)ctx; (void)sz;
	free(p);
}

static struct idx_entry *
__idx_lookup(struct bitcask *bc, const void *key, size_t key_len)
{
	uint64_t h = __hash_key(key, key_len);
	lh_node_t *n;
	struct idx_entry *e;
	for (n = *lh_slot(&bc->idx, h); n != NULL; n = n->next) {
		e = (struct idx_entry *)n;
		if (n->hash == h && e->key_len == key_len &&
		    memcmp(e->key, key, key_len) == 0)
			return e;
	}
	return NULL;
}

/* Insert/replace.  Updates bytes_dead with the prior entry's size
 * if any. */
static int
__idx_put(struct bitcask *bc, const void *key, size_t key_len,
          uint64_t offset, uint32_t val_len, uint64_t ts,
          const bitcask_meta_t *m)
{
	uint64_t h = __hash_key(key, key_len);
	struct idx_entry *e = __idx_lookup(bc, key, key_len);
	lh_node_t **slot;

	if (e != NULL) {
		bc->bytes_dead += __rec_size(e);
		e->offset = offset;
		e->val_len = val_len;
		e->timestamp = ts;
		e->tag = m->tag;
		e->expire_at = m->expire_at;
		return 0;
	}
	e = calloc(1, sizeof *e);
	if (e == NULL) return -1;
	e->key = malloc(key_len > 0 ? key_len : 1);   /* "" is a key */
	if (e->key == NULL) { free(e); return -1; }
	memcpy(e->key, key, key_len);
	e->key_len = key_len;
	e->offset = offset;
	e->val_len = val_len;
	e->timestamp = ts;
	e->tag = m->tag;
	e->expire_at = m->expire_at;
	e->node.hash = h;
	slot = lh_slot(&bc->idx, h);
	e->node.next = *slot;
	*slot = &e->node;
	/* Load factor 1: one bucket split per insert past it.  A refused
	 * split (out of memory) only leaves the chains a little longer. */
	if (++bc->n_keys > lh_buckets(&bc->idx))
		(void)lh_grow(&bc->idx, __idx_alloc, __idx_mfree, NULL);
	return 0;
}

static int
__idx_del(struct bitcask *bc, const void *key, size_t key_len)
{
	uint64_t h = __hash_key(key, key_len);
	lh_node_t **link, *n;
	struct idx_entry *e;
	for (link = lh_slot(&bc->idx, h); (n = *link) != NULL;
	    link = &n->next) {
		e = (struct idx_entry *)n;
		if (n->hash == h && e->key_len == key_len &&
		    memcmp(e->key, key, key_len) == 0) {
			*link = n->next;
			bc->bytes_dead += __rec_size(e);
			free(e->key); free(e);
			bc->n_keys--;
			return 0;
		}
	}
	return 1;
}

/* Drop every entry (a flush): each one's record becomes dead.  The
 * table keeps its size. */
static void
__idx_clear(struct bitcask *bc)
{
	size_t i;
	lh_node_t *n, *next;
	for (i = 0; i < lh_buckets(&bc->idx); i++) {
		for (n = *lh_bucket(&bc->idx, i); n != NULL; n = next) {
			next = n->next;
			bc->bytes_dead += __rec_size((struct idx_entry *)n);
			free(((struct idx_entry *)n)->key);
			free(n);
		}
		*lh_bucket(&bc->idx, i) = NULL;
	}
	bc->n_keys = 0;
}

static void
__idx_free_all(struct bitcask *bc)
{
	if (bc->idx.dir == NULL) return;
	__idx_clear(bc);
	lh_destroy(&bc->idx, __idx_mfree, NULL);
}

/* ---- recovery ----------------------------------------------- */

/*
 * Check a record header's key_len and val_len and compute how many
 * body bytes follow the header.  Returns -1 for a header no writer
 * produces.  An older log (`v1`) holds only plain puts and tombstones.
 */
static int
__rec_parse(uint32_t kl, uint32_t vl, int v1, uint32_t *body_len)
{
	if (v1 && vl != TOMBSTONE && vl > MAX_VAL_LEN)
		return -1;
	if (vl == REC_FLUSH) {
		*body_len = 0;
		return kl == 0 ? 0 : -1;
	}
	if (kl > MAX_VAL_LEN)
		return -1;
	if (vl == REC_BATCH) {
		*body_len = kl;
		return 0;
	}
	if (vl == TOMBSTONE) {
		*body_len = kl;
		return 0;
	}
	if ((vl & REC_V2) != 0) {
		vl &= ~REC_V2;
		if (vl > MAX_VAL_LEN)
			return -1;
		*body_len = kl + META_SIZE + vl;
		return 0;
	}
	if (vl > MAX_VAL_LEN)
		return -1;
	*body_len = kl + vl;
	return 0;
}

/*
 * Apply one verified record, found at file offset `off`, to the index.
 * A flush empties the index, a tombstone erases its key, and a put
 * whose expiry is at or before `now` (wall ns) erases its key too: the
 * key expired while nobody was serving it and must not come back.
 */
static void
__rec_apply(struct bitcask *bc, uint64_t off, uint64_t ts, uint32_t kl,
    uint32_t vl, const uint8_t *body, int64_t now)
{
	bitcask_meta_t m = { 0, 0 };
	uint32_t hs = bc->hdr_size;
	uint64_t voff = off + hs + kl;

	if (vl == REC_FLUSH) {
		__idx_clear(bc);
		bc->bytes_dead += hs;
		return;
	}
	if (vl == TOMBSTONE) {
		(void)__idx_del(bc, body, kl);
		bc->bytes_dead += hs + kl;
		return;
	}
	if ((vl & REC_V2) != 0) {
		vl &= ~REC_V2;
		m.tag = body[kl];
		m.expire_at = (int64_t)get_u64(body + kl + 1);
		voff += META_SIZE;
	}
	if (m.expire_at != 0 && m.expire_at <= now) {
		(void)__idx_del(bc, body, kl);
		bc->bytes_dead += hs + kl + META_SIZE + vl;
		return;
	}
	(void)__idx_put(bc, body, kl, voff, vl, ts, &m);
}

/*
 * Apply a batch record's body (`len` bytes, already checked against
 * the batch's CRC), found at file offset `off`.  The body holds whole
 * records, none of them a batch.  Returns 0, or -1 for a body no writer
 * produces: the open then fails rather than guess.
 */
static int
__batch_apply(struct bitcask *bc, uint64_t off, const uint8_t *body,
    uint32_t len, int64_t now)
{
	uint32_t pos = 0, kl, vl, blen;

	while (pos < len) {
		if (len - pos < HDR_SIZE)
			return -1;
		kl = get_u32(body + pos + 16);
		vl = get_u32(body + pos + 20);
		if (vl == REC_BATCH || __rec_parse(kl, vl, 0, &blen) != 0 ||
		    len - pos - HDR_SIZE < blen)
			return -1;
		__rec_apply(bc, off + pos, get_u64(body + pos + 8), kl, vl,
		    body + pos + HDR_SIZE, now);
		pos += HDR_SIZE + blen;
	}
	bc->bytes_dead += HDR_SIZE;           /* the batch's own header */
	return 0;
}

/* The name of the file being read: the older log while converting. */
static const char *
__file_of(const struct bitcask *bc)
{
	return bc->hdr_size == HDR_V1 ? V1_FILE : DATA_FILE;
}

/* Cut a torn tail off at `off`.  Returns 0, or -1 if the file cannot
 * be truncated (the open then fails). */
static int
__cut_tail(struct bitcask *bc, uint64_t off, uint64_t fsize)
{
	if (fsize > off)
		fprintf(stderr, "bitcask: %s/%s: cut a torn tail "
		    "of %llu bytes at offset %llu\n", bc->dir, __file_of(bc),
		    (unsigned long long)(fsize - off),
		    (unsigned long long)off);
	return ftruncate(bc->fd, (off_t)off) == 0 ? 0 : -1;
}

/* What __rec_read found at an offset. */
enum rec_state {
	REC_OK,         /* a whole record with good CRCs */
	REC_TORN,       /* a good header whose record runs past EOF */
	REC_BAD,        /* a bad header CRC, a header no writer produces,
	                   or a bad body CRC */
	REC_IOERR       /* a read failed, or out of memory */
};

/*
 * Read the record at `off` of a file of `fsize` bytes: its header into
 * hdr (HDR_SIZE bytes, in the v2 layout), its body into *body (grown as
 * needed; *cap is its size, and it is never left NULL on REC_OK, so an
 * empty key has a valid pointer), its body length into *body_len.  At
 * least bc->hdr_size bytes must remain at `off`.
 *
 * The header CRC is checked first, so the lengths are believed only
 * when they are what was written: REC_TORN then means a header that
 * checks out with too few bytes after it, which is what a crash in the
 * middle of the last write leaves.  An older log (bc->hdr_size is
 * HDR_V1) has no header CRC: its 20-byte header is read into hdr + 4,
 * and its one CRC starts from seed 0 over the header fields.
 */
static enum rec_state
__rec_read(struct bitcask *bc, uint64_t off, uint64_t fsize, uint8_t *hdr,
    uint8_t **body, size_t *cap, uint32_t *body_len)
{
	uint32_t hs = bc->hdr_size, crc;
	int v1 = hs == HDR_V1;
	void *nb;

	if (pread(bc->fd, hdr + HDR_SIZE - hs, hs, (off_t)off) != (ssize_t)hs)
		return REC_IOERR;
	if (v1)
		crc = crc32(hdr + 8, HDR_SIZE - 8, 0);
	else if (__hdr_ok(hdr, SEED_TOP))
		crc = get_u32(hdr);
	else
		return REC_BAD;
	if (__rec_parse(get_u32(hdr + 16), get_u32(hdr + 20), v1,
	    body_len) != 0)
		return REC_BAD;
	if (fsize - off - hs < *body_len)
		return REC_TORN;
	if (*body_len > *cap || *body == NULL) {
		nb = realloc(*body, *body_len > 0 ? *body_len : 1);
		if (nb == NULL)
			return REC_IOERR;
		*body = nb;
		*cap = *body_len;
	}
	if (*body_len > 0 && pread(bc->fd, *body, *body_len,
	    (off_t)(off + hs)) != (ssize_t)*body_len)
		return REC_IOERR;
	crc = crc32(*body, *body_len, crc);
	return crc == get_u32(hdr + 4) ? REC_OK : REC_BAD;
}

#define SCAN_BLOCK (1024u * 1024u)

/* Test hook (bitcask.h): when it counts down to zero, the damage scan's
 * allocation or read fails. */
int bitcask_scan_fail_at;

/* Whether the scan's next allocation or read must fail (the hook). */
static int
__scan_fault(void)
{
	return bitcask_scan_fail_at > 0 && --bitcask_scan_fail_at == 0;
}

/*
 * Check, in memory, whether a whole, valid record starts at `p`, with
 * `avail` bytes of the scan's block and `left` bytes of the file from
 * there (at least the header's size in both).  REC_OK if one does,
 * REC_BAD if none can, and REC_TORN if the header passes but the body
 * runs past the block: the caller then reads the record from the file.
 * A body that runs past the end of the file is REC_BAD here: a torn
 * record is not a valid one.
 */
static enum rec_state
__scan_at(const struct bitcask *bc, const uint8_t *p, size_t avail,
    uint64_t left)
{
	uint32_t hs = bc->hdr_size, body_len, crc;

	if (hs == HDR_SIZE && !__hdr_ok(p, SEED_TOP))
		return REC_BAD;
	if (__rec_parse(get_u32(p + hs - 8), get_u32(p + hs - 4),
	    hs == HDR_V1, &body_len) != 0 || left - hs < body_len)
		return REC_BAD;
	if (avail - hs < body_len)
		return REC_TORN;
	crc = hs == HDR_V1 ? crc32(p + 4, HDR_V1 - 4, 0) : get_u32(p);
	crc = crc32(p + hs, body_len, crc);
	return crc == get_u32(p + hs - 20) ? REC_OK : REC_BAD;
}

/*
 * Find the first offset after `bad` at which a whole, valid record
 * starts.  Returns 1 and sets *next if there is one, 0 if there is
 * none, and -1 if a read or an allocation failed: the caller must then
 * fail the open and leave the file alone, since it cannot know that
 * nothing valid follows.  A crash leaves a torn record only at the end
 * of the log, so a bad record with a valid one after it is damage.
 *
 * The file is read in SCAN_BLOCK blocks and every byte offset is
 * checked in memory (__scan_at); only a record that runs past the
 * block is read from the file.  A random 16 bytes pass the header CRC
 * with odds of 2^-32, so a damaged tail costs about one CRC of 16 bytes
 * per byte.  An older log has no header CRC: every offset whose
 * lengths parse is a candidate, and its body CRC decides.
 */
static int
__next_valid(struct bitcask *bc, uint64_t bad, uint64_t fsize,
    uint64_t *next)
{
	uint8_t hdr[HDR_SIZE], *blk, *body = NULL;
	size_t cap = 0, n, i;
	uint32_t body_len, hs = bc->hdr_size;
	uint64_t q = bad + 1;
	enum rec_state st;
	int rc = 0;

	if (__scan_fault() || (blk = malloc(SCAN_BLOCK)) == NULL)
		return -1;
	while (rc == 0 && fsize - q >= hs) {
		n = fsize - q < SCAN_BLOCK ? (size_t)(fsize - q) : SCAN_BLOCK;
		if (__scan_fault() ||
		    pread(bc->fd, blk, n, (off_t)q) != (ssize_t)n) {
			rc = -1;
			break;
		}
		for (i = 0; i + hs <= n; i++) {
			st = __scan_at(bc, blk + i, n - i, fsize - q - i);
			if (st == REC_TORN)
				st = __rec_read(bc, q + i, fsize, hdr, &body,
				    &cap, &body_len);
			if (st == REC_IOERR) {
				rc = -1;
				break;
			}
			if (st == REC_OK) {
				*next = q + i;
				rc = 1;
				break;
			}
		}
		q += i;
	}
	free(body);
	free(blk);
	return rc;
}

/*
 * Rebuild the index from the data file.  A torn tail (a crash in the
 * middle of the last write) is cut off, and so is a bad record with no
 * valid record after it, since it cannot be told from one.  A bad
 * record with a valid record after it refuses the open with the
 * offset: cutting there would silently drop every later record.  In an
 * older log a torn record is treated as a bad one, since its lengths
 * have no CRC.  Returns 0 or -1.
 */
static int
__recover(struct bitcask *bc)
{
	off_t fsize = lseek(bc->fd, 0, SEEK_END);
	uint64_t off = 0, next, fs;
	uint8_t  hdr[HDR_SIZE];
	uint8_t *body = NULL;
	size_t   body_cap = 0;
	uint32_t body_len, kl, vl;
	enum rec_state st;
	int      rc = 0;
	int64_t  now = wall_ns();

	if (fsize < 0) return -1;
	fs = (uint64_t)fsize;

	while (off < fs) {
		if (fs - off < bc->hdr_size) {
			rc = __cut_tail(bc, off, fs);   /* a torn header */
			break;
		}
		st = __rec_read(bc, off, fs, hdr, &body, &body_cap, &body_len);
		if (st == REC_IOERR) {
			rc = -1;
			break;
		}
		if (st == REC_TORN && bc->hdr_size == HDR_SIZE) {
			rc = __cut_tail(bc, off, fs);
			break;
		}
		/* An older log's lengths have no CRC, so a record there that
		 * runs past the end of the file may be a damaged length: it
		 * is cut only if nothing valid follows, as a bad record is. */
		if (st != REC_OK) {
			if ((rc = __next_valid(bc, off, fs, &next)) == 0) {
				rc = __cut_tail(bc, off, fs);
				break;
			}
			if (rc < 0) {
				fprintf(stderr, "bitcask: %s/%s: the record "
				    "at offset %llu is corrupt, and the search "
				    "for a valid record after it failed (a read "
				    "or out of memory); refusing to open\n",
				    bc->dir, __file_of(bc),
				    (unsigned long long)off);
				break;
			}
			fprintf(stderr, "bitcask: %s/%s: the record "
			    "at offset %llu is corrupt and a valid record "
			    "follows at offset %llu; refusing to open (the "
			    "log is damaged, not torn: restore it, or cut it "
			    "at %llu by hand to drop everything from there)\n",
			    bc->dir, __file_of(bc), (unsigned long long)off,
			    (unsigned long long)next, (unsigned long long)off);
			rc = -1;
			break;
		}
		kl = get_u32(hdr + 16);
		vl = get_u32(hdr + 20);
		if (vl != REC_BATCH) {
			__rec_apply(bc, off, get_u64(hdr + 8), kl, vl, body,
			    now);
		} else if (__batch_apply(bc, off + HDR_SIZE, body, body_len,
		    now) != 0) {
			fprintf(stderr, "bitcask: %s/%s: malformed "
			    "batch record at offset %llu\n", bc->dir, __file_of(bc),
			    (unsigned long long)off);
			rc = -1;
			break;
		}
		off += bc->hdr_size + body_len;
	}
	bc->write_offset = off;
	bc->bytes_used = off;           /* every record kept is in use */
	free(body);
	return rc;
}

/* ---- public API --------------------------------------------- */

/* fsync the directory, so a rename or unlink in it is durable.
 * Returns 0 or -1. */
static int
__fsync_dir(const char *dir)
{
	int dfd, rc;

	if ((dfd = open(dir, O_RDONLY)) < 0)
		return -1;
	rc = fsync(dfd);
	(void)close(dfd);
	return rc == 0 ? 0 : -1;
}

/*
 * Check the older log's files and say what bitcask_open must do with
 * this directory: 0 open bitcask.v2, 1 convert bitcask.v1 into it, or
 * -1 refuse (or a rename, unlink or directory fsync failed).
 *
 * The conversion runs in steps, each durable before the next, so a
 * crash at any point resumes at the next open:
 *
 *   bitcask.data alone             renamed to bitcask.v1, then 1.
 *   bitcask.v1, no bitcask.v2      1.  A temp file a crash left in the
 *                                  middle of the merge is removed first.
 *   bitcask.v1 and bitcask.v2      bitcask.v2 appears only by the
 *                                  merge's rename, so it is whole:
 *                                  bitcask.v1 is removed, then 0.
 *   bitcask.data and bitcask.v1    refused: no step leaves both.
 *
 * An older binary pointed at a converted directory finds no
 * bitcask.data and starts empty, but creates an empty one.  An EMPTY
 * bitcask.data next to bitcask.v2 holds nothing it acknowledged, so it
 * is removed, with a warning.  One with data holds writes neither file
 * can drop silently, so the open is refused, naming both files.
 */
static int
__adopt_v1(const char *dir)
{
	char data[512], v1[512], v2[512], tmp[512];
	struct stat sb;
	int has_data, has_v1, has_v2;

	snprintf(data, sizeof data, "%s/bitcask.data", dir);
	snprintf(v1, sizeof v1, "%s/" V1_FILE, dir);
	snprintf(v2, sizeof v2, "%s/" DATA_FILE, dir);
	snprintf(tmp, sizeof tmp, "%s/" TMP_FILE, dir);
	has_data = stat(data, &sb) == 0;
	has_v1 = access(v1, F_OK) == 0;
	has_v2 = access(v2, F_OK) == 0;

	if (has_data && has_v1) {
		fprintf(stderr, "bitcask: %s and %s (an older log in the "
		    "middle of its conversion) both exist; refusing to pick "
		    "one: move one away\n", data, v1);
		return -1;
	}
	if (has_data && has_v2) {
		if (sb.st_size != 0) {
			fprintf(stderr, "bitcask: %s (%lld bytes, written by "
			    "an older rexis) and %s both exist; refusing to "
			    "pick one: move one away\n", data,
			    (long long)sb.st_size, v2);
			return -1;
		}
		fprintf(stderr, "bitcask: removing the empty %s an older "
		    "rexis left next to %s\n", data, v2);
		if (unlink(data) != 0)
			return -1;
		return __fsync_dir(dir);
	}
	if (has_data) {
		if (rename(data, v1) != 0 || __fsync_dir(dir) != 0)
			return -1;
		has_v1 = 1;
	}
	if (!has_v1)
		return 0;
	if (has_v2) {
		if (unlink(v1) != 0 || __fsync_dir(dir) != 0)
			return -1;
		return 0;
	}
	if (unlink(tmp) != 0 && errno != ENOENT)
		return -1;
	return 1;
}

/*
 * Convert the older log: read bitcask.v1 with its 20-byte headers, write
 * the live keys to bitcask.v2 with the merge, then remove bitcask.v1.
 * On return bc holds bitcask.v2 open.  Returns 0 or -1.
 *
 * The torn and damaged rules are those of bitcask.v2, except that an
 * older header has no CRC: a record whose lengths run past the end of
 * the file may be a damaged length, so it is cut only if no valid
 * record follows it (see __recover).  A refusal leaves bitcask.v1 as
 * it was, and the next open tries the conversion again.
 */
static int
__convert_v1(struct bitcask *bc)
{
	char v1[512];

	snprintf(v1, sizeof v1, "%s/" V1_FILE, bc->dir);
	if ((bc->fd = open(v1, O_RDWR)) < 0)
		return -1;
	bc->hdr_size = HDR_V1;
	if (__recover(bc) != 0)
		return -1;
	bc->hdr_size = HDR_SIZE;
	if (bitcask_compact(bc) != 0)
		return -1;
	fprintf(stderr, "bitcask: converted %s to %s/" DATA_FILE "\n", v1,
	    bc->dir);
	if (unlink(v1) != 0 || __fsync_dir(bc->dir) != 0)
		return -1;
	return 0;
}

int
bitcask_open(const char *dir, bitcask_t **out)
{
	bitcask_t *bc;
	char path[512];
	int rc;

	if (dir == NULL || out == NULL) return -1;
	bc = calloc(1, sizeof *bc);
	if (bc == NULL) return -1;
	if (lh_init(&bc->idx, IDX_BUCKETS, __idx_alloc, __idx_mfree,
	    NULL) != 0) {
		free(bc);
		return -1;
	}
	pthread_mutex_init(&bc->lock, NULL);
	snprintf(bc->dir, sizeof bc->dir, "%s", dir);
	snprintf(path, sizeof path, "%s/" DATA_FILE, dir);
	bc->fd = -1;
	bc->hdr_size = HDR_SIZE;
	rc = __adopt_v1(dir);
	if (rc == 1)
		rc = __convert_v1(bc);
	else if (rc == 0 && (bc->fd = open(path, O_RDWR | O_CREAT, 0644)) >= 0)
		rc = __recover(bc);
	else
		rc = -1;
	if (rc != 0) {
		if (bc->fd >= 0)
			(void)close(bc->fd);
		__idx_free_all(bc);
		pthread_mutex_destroy(&bc->lock);
		free(bc);
		return -1;
	}
	*out = bc;
	return 0;
}

void
bitcask_close(bitcask_t *bc)
{
	if (bc == NULL) return;
	(void)fsync(bc->fd);
	(void)close(bc->fd);
	__idx_free_all(bc);
	pthread_mutex_destroy(&bc->lock);
	free(bc);
}

/*
 * Append one record (header, key, the optional meta, the value) at the
 * write offset with one pwritev, under the lock, and advance the
 * offset.  `extra` is the bytes after the key.  A short write leaves
 * the offset alone: the next record overwrites the torn one, and a
 * crash before that leaves a tail recovery truncates.
 */
static int
__rec_append(struct bitcask *bc, struct iovec *iov, int iovcnt,
    size_t total)
{
	ssize_t n;
	n = pwritev(bc->fd, iov, iovcnt, (off_t)bc->write_offset);
	if (n != (ssize_t)total)
		return -1;
	bc->write_offset += total;
	return 0;
}

int
bitcask_put(bitcask_t *bc, const void *key, size_t key_len,
            const void *val, size_t val_len)
{
	bitcask_meta_t m = { 0, 0 };
	return bitcask_put_meta(bc, key, key_len, val, val_len, &m);
}

int
bitcask_put_meta(bitcask_t *bc, const void *key, size_t key_len,
                 const void *val, size_t val_len, const bitcask_meta_t *m)
{
	uint8_t hdr[HDR_SIZE], meta[META_SIZE];
	struct iovec iov[4];
	uint64_t ts = (uint64_t)now_ns();
	uint32_t crc, vl;
	size_t meta_len = 0, total;
	uint64_t voff;
	int rc;

	if (bc == NULL || key == NULL || (val == NULL && val_len > 0) ||
	    m == NULL || key_len > MAX_VAL_LEN || val_len > MAX_VAL_LEN)
		return -1;

	/* A plain put (tag 0, no expiry) keeps the shorter v1 format. */
	vl = (uint32_t)val_len;
	if (m->tag != 0 || m->expire_at != 0) {
		vl |= REC_V2;
		meta[0] = m->tag;
		put_u64(meta + 1, (uint64_t)m->expire_at);
		meta_len = META_SIZE;
	}

	crc = __hdr_seal(hdr, ts, (uint32_t)key_len, vl, SEED_TOP);
	crc = crc32(key, key_len, crc);
	crc = crc32(meta, meta_len, crc);
	crc = crc32(val, val_len, crc);
	put_u32(hdr + 4, crc);

	iov[0].iov_base = hdr;          iov[0].iov_len = HDR_SIZE;
	iov[1].iov_base = (void *)key;  iov[1].iov_len = key_len;
	iov[2].iov_base = meta;         iov[2].iov_len = meta_len;
	iov[3].iov_base = (void *)val;  iov[3].iov_len = val_len;
	total = HDR_SIZE + key_len + meta_len + val_len;

	pthread_mutex_lock(&bc->lock);
	voff = bc->write_offset + HDR_SIZE + key_len + meta_len;
	rc = __rec_append(bc, iov, 4, total);
	if (rc == 0) {
		rc = __idx_put(bc, key, key_len, voff, (uint32_t)val_len,
		    ts, m);
		bc->bytes_used += total;
		bc->puts++;
	}
	pthread_mutex_unlock(&bc->lock);
	return rc;
}

int
bitcask_get(bitcask_t *bc, const void *key, size_t key_len,
            void *val_out, size_t val_buf_size, size_t *val_len)
{
	struct idx_entry *e;
	ssize_t n;
	size_t copy_len;
	if (bc == NULL || key == NULL || val_len == NULL) return -1;
	pthread_mutex_lock(&bc->lock);
	bc->gets++;
	e = __idx_lookup(bc, key, key_len);
	if (e == NULL) {
		bc->misses++;
		pthread_mutex_unlock(&bc->lock);
		*val_len = 0;
		return 1;
	}
	bc->hits++;
	*val_len = e->val_len;
	copy_len = (e->val_len < val_buf_size) ? e->val_len : val_buf_size;
	pthread_mutex_unlock(&bc->lock);
	if (copy_len > 0 && val_out != NULL) {
		n = pread(bc->fd, val_out, copy_len, (off_t)e->offset);
		if (n != (ssize_t)copy_len) return -1;
	}
	return 0;
}

int
bitcask_size(bitcask_t *bc, const void *key, size_t key_len, size_t *val_len)
{
	struct idx_entry *e;
	if (bc == NULL || key == NULL || val_len == NULL) return -1;
	pthread_mutex_lock(&bc->lock);
	e = __idx_lookup(bc, key, key_len);
	if (e == NULL) {
		pthread_mutex_unlock(&bc->lock);
		*val_len = 0;
		return 1;
	}
	*val_len = e->val_len;
	pthread_mutex_unlock(&bc->lock);
	return 0;
}

/*
 * Fill hdr with the header of a record whose body is `body` (`len`
 * bytes) and whose key_len and val_len fields are kl and kind: a
 * tombstone (kl = len, the key), a flush (no body) or a batch (kl =
 * len, the inner records).  `seed` is SEED_TOP, or SEED_INNER for a
 * record inside a batch.
 */
static void
__marker_hdr(uint8_t *hdr, const void *body, size_t len, uint32_t kl,
    uint32_t kind, uint32_t seed)
{
	uint32_t crc;

	crc = __hdr_seal(hdr, (uint64_t)now_ns(), kl, kind, seed);
	put_u32(hdr + 4, crc32(body, len, crc));
}

/* Append a record with no value bytes: a tombstone (key) or a flush
 * (no key).  Returns 0 or -1. */
static int
__rec_marker(struct bitcask *bc, const void *key, size_t key_len,
    uint32_t kind)
{
	uint8_t hdr[HDR_SIZE];
	struct iovec iov[2];

	__marker_hdr(hdr, key, key_len, (uint32_t)key_len, kind, SEED_TOP);
	iov[0].iov_base = hdr;          iov[0].iov_len = HDR_SIZE;
	iov[1].iov_base = (void *)key;  iov[1].iov_len = key_len;
	if (__rec_append(bc, iov, 2, HDR_SIZE + key_len) != 0)
		return -1;
	bc->bytes_used += HDR_SIZE + key_len;
	bc->bytes_dead += HDR_SIZE + key_len;   /* a marker is never live */
	return 0;
}

int
bitcask_meta(bitcask_t *bc, const void *key, size_t key_len,
             bitcask_meta_t *out)
{
	struct idx_entry *e;
	if (bc == NULL || key == NULL || out == NULL) return -1;
	pthread_mutex_lock(&bc->lock);
	e = __idx_lookup(bc, key, key_len);
	if (e != NULL) {
		out->tag = e->tag;
		out->expire_at = e->expire_at;
	}
	pthread_mutex_unlock(&bc->lock);
	return e != NULL ? 0 : 1;
}

int
bitcask_del(bitcask_t *bc, const void *key, size_t key_len)
{
	int found;
	if (bc == NULL || key == NULL || key_len > MAX_VAL_LEN)
		return -1;

	pthread_mutex_lock(&bc->lock);
	bc->dels++;
	found = (__idx_lookup(bc, key, key_len) != NULL);
	if (__rec_marker(bc, key, key_len, TOMBSTONE) != 0) {
		pthread_mutex_unlock(&bc->lock);
		return -1;
	}
	if (found) __idx_del(bc, key, key_len);
	pthread_mutex_unlock(&bc->lock);
	return found ? 0 : 1;
}

/*
 * Delete several keys with one batch record (one pwritev): a crash
 * keeps every tombstone or none.  The record is built in one buffer:
 * the batch header, then a tombstone per key.
 */
int
bitcask_del_many(bitcask_t *bc, const void *const *keys,
    const size_t *lens, size_t n)
{
	uint8_t *buf, *p;
	struct iovec iov[1];
	size_t i, inner = 0;
	int rc;

	if (bc == NULL || keys == NULL || lens == NULL)
		return -1;
	if (n == 1)
		return bitcask_del(bc, keys[0], lens[0]) < 0 ? -1 : 0;
	for (i = 0; i < n; i++) {
		if (keys[i] == NULL || lens[i] > MAX_VAL_LEN)
			return -1;
		inner += HDR_SIZE + lens[i];
		if (inner > MAX_VAL_LEN)
			return -1;
	}
	if (n == 0)
		return 0;
	if ((buf = malloc(HDR_SIZE + inner)) == NULL)
		return -1;

	/* The tombstones, then the batch header over them. */
	p = buf + HDR_SIZE;
	for (i = 0; i < n; i++) {
		__marker_hdr(p, keys[i], lens[i], (uint32_t)lens[i],
		    TOMBSTONE, SEED_INNER);
		memcpy(p + HDR_SIZE, keys[i], lens[i]);
		p += HDR_SIZE + lens[i];
	}
	__marker_hdr(buf, buf + HDR_SIZE, inner, (uint32_t)inner, REC_BATCH,
	    SEED_TOP);
	iov[0].iov_base = buf;
	iov[0].iov_len = HDR_SIZE + inner;

	pthread_mutex_lock(&bc->lock);
	rc = __rec_append(bc, iov, 1, HDR_SIZE + inner);
	if (rc == 0) {
		for (i = 0; i < n; i++)
			(void)__idx_del(bc, keys[i], lens[i]);
		bc->bytes_used += HDR_SIZE + inner;
		bc->bytes_dead += HDR_SIZE + inner;   /* never live */
		bc->dels += n;
	}
	pthread_mutex_unlock(&bc->lock);
	free(buf);
	return rc;
}

int
bitcask_flush(bitcask_t *bc)
{
	int rc;
	if (bc == NULL) return -1;
	pthread_mutex_lock(&bc->lock);
	rc = __rec_marker(bc, "", 0, REC_FLUSH);
	if (rc == 0)
		__idx_clear(bc);
	pthread_mutex_unlock(&bc->lock);
	return rc;
}

int
bitcask_iterate(bitcask_t *bc, bitcask_iter_fn fn, void *user)
{
	size_t i;
	size_t nkeys = 0, capacity = 0;
	struct snap { uint8_t *key; size_t key_len; };
	struct snap *snap = NULL;
	int rc = 0;

	if (bc == NULL || fn == NULL) return -1;

	/* Phase 1: snapshot all (key, key_len) pairs while holding the
	 * lock.  Releasing the lock before invoking user callbacks lets
	 * those callbacks safely re-enter bitcask_get / bitcask_size /
	 * bitcask_put without deadlocking on this mutex. */
	pthread_mutex_lock(&bc->lock);
	capacity = bc->n_keys;
	if (capacity > 0) {
		snap = calloc(capacity, sizeof *snap);
		if (snap == NULL) {
			pthread_mutex_unlock(&bc->lock);
			return -1;
		}
		for (i = 0; i < lh_buckets(&bc->idx) && nkeys < capacity;
		    i++) {
			struct idx_entry *e;
			for (e = (struct idx_entry *)*lh_bucket(&bc->idx, i);
			    e != NULL && nkeys < capacity;
			    e = (struct idx_entry *)e->node.next) {
				snap[nkeys].key =
				    malloc(e->key_len > 0 ? e->key_len : 1);
				if (snap[nkeys].key == NULL) {
					size_t j;
					for (j = 0; j < nkeys; j++)
						free(snap[j].key);
					free(snap);
					pthread_mutex_unlock(&bc->lock);
					return -1;
				}
				memcpy(snap[nkeys].key, e->key, e->key_len);
				snap[nkeys].key_len = e->key_len;
				nkeys++;
			}
		}
	}
	pthread_mutex_unlock(&bc->lock);

	/* Phase 2: dispatch callbacks without holding the lock. */
	for (i = 0; i < nkeys; i++) {
		if (rc == 0 && fn(snap[i].key, snap[i].key_len, user) != 0)
			rc = 0; /* user requested early stop; not an error */
		free(snap[i].key);
	}
	free(snap);
	return 0;
}

int
bitcask_sync(bitcask_t *bc)
{
	if (bc == NULL) return -1;
	return fsync(bc->fd) == 0 ? 0 : -1;
}

/*
 * Copy one live entry's record into the compacted file at `*off` and
 * re-point the entry there.  The value is read back from the current
 * data file; the meta (tag, expiry) comes from the index.  Returns 0
 * or -1.
 */
static int
__compact_one(struct bitcask *bc, int tfd, struct idx_entry *e,
    uint64_t *off)
{
	uint8_t hdr[HDR_SIZE], meta[META_SIZE];
	struct iovec iov[4];
	uint8_t *val = NULL;
	size_t meta_len = 0, total;
	uint32_t crc, vl = e->val_len;
	ssize_t n;

	if (e->val_len > 0) {
		val = malloc(e->val_len);
		if (val == NULL) return -1;
		if (pread(bc->fd, val, e->val_len, (off_t)e->offset) !=
		    (ssize_t)e->val_len) {
			free(val);
			return -1;
		}
	}
	if (e->tag != 0 || e->expire_at != 0) {
		vl |= REC_V2;
		meta[0] = e->tag;
		put_u64(meta + 1, (uint64_t)e->expire_at);
		meta_len = META_SIZE;
	}
	crc = __hdr_seal(hdr, e->timestamp, (uint32_t)e->key_len, vl,
	    SEED_TOP);
	crc = crc32(e->key, e->key_len, crc);
	crc = crc32(meta, meta_len, crc);
	crc = crc32(val, e->val_len, crc);
	put_u32(hdr + 4, crc);

	iov[0].iov_base = hdr;     iov[0].iov_len = HDR_SIZE;
	iov[1].iov_base = e->key;  iov[1].iov_len = e->key_len;
	iov[2].iov_base = meta;    iov[2].iov_len = meta_len;
	iov[3].iov_base = val;     iov[3].iov_len = e->val_len;
	total = HDR_SIZE + e->key_len + meta_len + e->val_len;
	n = pwritev(tfd, iov, 4, (off_t)*off);
	free(val);
	if (n != (ssize_t)total)
		return -1;
	e->offset = *off + HDR_SIZE + e->key_len + meta_len;
	*off += total;
	return 0;
}

/*
 * Compaction (merge).  Rewrite the data file keeping only the live
 * records -- the current index entries -- so superseded versions,
 * deleted keys, flushed keys and the tombstone and flush records
 * themselves (the bytes_dead bytes) are reclaimed.  A live entry keeps
 * its tag and expiry; an entry whose expiry has passed is dropped from
 * the index and not copied.  The live set is written to a temp file,
 * fsync'd, then renamed over the data file, and the directory is
 * fsync'd; the in-memory index offsets are re-pointed into the new file
 * as each record is written.
 * Holds the write lock for the whole pass (a single-file Bitcask
 * cannot serve reads from the old file while the new one is being
 * built); a multi-file variant would merge immutable segments in the
 * background instead.
 */
int
bitcask_compact(bitcask_t *bc)
{
	char tmp_path[512], data_path[512];
	int tfd, dir_rc;
	uint64_t new_off = 0;
	int64_t now = wall_ns();
	size_t i;
	int rc = -1;

	if (bc == NULL) return -1;

	snprintf(tmp_path, sizeof tmp_path, "%s/" TMP_FILE, bc->dir);
	snprintf(data_path, sizeof data_path, "%s/" DATA_FILE, bc->dir);

	pthread_mutex_lock(&bc->lock);

	tfd = open(tmp_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (tfd < 0) goto out;

	for (i = 0; i < lh_buckets(&bc->idx); i++) {
		lh_node_t **link = lh_bucket(&bc->idx, i);
		while (*link != NULL) {
			struct idx_entry *e = (struct idx_entry *)*link;

			/* Expired: unlink and free it; nothing is copied. */
			if (e->expire_at != 0 && e->expire_at <= now) {
				*link = e->node.next;
				free(e->key);
				free(e);
				bc->n_keys--;
				continue;
			}
			if (__compact_one(bc, tfd, e, &new_off) != 0)
				goto fail;
			link = &e->node.next;
		}
	}

	if (fsync(tfd) != 0) goto fail;
	if (rename(tmp_path, data_path) != 0) goto fail;
	/* Make the rename durable: without it a power loss after the
	 * merge can bring back the old file under the data file's name.
	 * The new file is in place by now, so a failure here leaves the
	 * handle consistent; it is reported, not undone. */
	dir_rc = __fsync_dir(bc->dir);

	/* Swap to the compacted file. */
	(void)close(bc->fd);
	bc->fd = tfd;
	bc->write_offset = new_off;
	bc->bytes_used = new_off;
	bc->bytes_dead = 0;
	rc = dir_rc;
	pthread_mutex_unlock(&bc->lock);
	return rc;

fail:
	(void)close(tfd);
	(void)unlink(tmp_path);
	/* Index offsets may be partially re-pointed at the temp file; the
	 * old data file is untouched and still authoritative, but the
	 * in-memory offsets are now inconsistent, so the caller must
	 * treat a failed compaction as fatal for this handle. */
out:
	pthread_mutex_unlock(&bc->lock);
	return rc;
}

void
bitcask_stat(bitcask_t *bc, bitcask_stats_t *out)
{
	if (bc == NULL || out == NULL) return;
	pthread_mutex_lock(&bc->lock);
	out->n_keys     = bc->n_keys;
	out->bytes_used = bc->bytes_used;
	out->bytes_dead = bc->bytes_dead;
	out->puts       = bc->puts;
	out->gets       = bc->gets;
	out->dels       = bc->dels;
	out->hits       = bc->hits;
	out->misses     = bc->misses;
	pthread_mutex_unlock(&bc->lock);
}
