/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/05_rexis/bitcask.c -- Bitcask-style on-disk KV.
 *
 * On-disk record format (network byte order; CRC32 over body):
 *
 *   uint32_t  crc;        // CRC32 of (timestamp .. value)
 *   uint64_t  timestamp;  // monotonic ns of write
 *   uint32_t  key_len;
 *   uint32_t  val_len;    // the record kind, see below
 *   uint8_t   key[key_len];
 *   uint8_t   meta[9];    // v2 put only: tag, then expire_at (u64)
 *   uint8_t   val[len];   // puts only
 *
 * val_len says which kind of record it is:
 *
 *   0 .. BITCASK_MAX_VAL     v1 put: val_len value bytes, no meta.
 *                            The original format; a put with tag 0
 *                            and no expiry is still written this way.
 *   REC_V2 | len             v2 put: meta, then len value bytes.
 *   REC_FLUSH (0xFFFFFFFE)   flush: key_len 0, no body.  Every
 *                            earlier record is dead.
 *   REC_BATCH (0xFFFFFFFD)   batch: key_len is the length of the
 *                            body, which is whole records of the
 *                            other kinds, back to back.  The CRC
 *                            covers the body, so recovery applies a
 *                            batch whole or cuts it off whole.
 *   REC_TOMBSTONE (~0)       delete: key only.
 *
 * A binary that predates v2 and flush records would read either as a
 * torn tail and truncate the log there, so the file has a new name
 * (bitcask.v2, see bitcask.h): an older binary never opens it.
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

#define HDR_SIZE  (4 /*crc*/ + 8 /*ts*/ + 4 /*kl*/ + 4 /*vl*/)
#define META_SIZE (1 /*tag*/ + 8 /*expire_at*/)
#define TOMBSTONE 0xffffffffu
#define REC_FLUSH 0xfffffffeu
#define REC_BATCH 0xfffffffdu
#define REC_V2    0x80000000u
#define DATA_FILE "bitcask.v2"

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
 * produces (a torn or corrupt tail): the caller truncates there.
 */
static int
__rec_parse(uint32_t kl, uint32_t vl, uint32_t *body_len)
{
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
	uint64_t voff = off + HDR_SIZE + kl;

	if (vl == REC_FLUSH) {
		__idx_clear(bc);
		bc->bytes_dead += HDR_SIZE;
		return;
	}
	if (vl == TOMBSTONE) {
		(void)__idx_del(bc, body, kl);
		bc->bytes_dead += HDR_SIZE + kl;
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
		bc->bytes_dead += HDR_SIZE + kl + META_SIZE + vl;
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
		kl = get_u32(body + pos + 12);
		vl = get_u32(body + pos + 16);
		if (vl == REC_BATCH || __rec_parse(kl, vl, &blen) != 0 ||
		    len - pos - HDR_SIZE < blen)
			return -1;
		__rec_apply(bc, off + pos, get_u64(body + pos + 4), kl, vl,
		    body + pos + HDR_SIZE, now);
		pos += HDR_SIZE + blen;
	}
	bc->bytes_dead += HDR_SIZE;           /* the batch's own header */
	return 0;
}

/* Cut a torn or corrupt tail off at `off`.  Returns 0, or -1 if the
 * file cannot be truncated (the open then fails). */
static int
__cut_tail(struct bitcask *bc, uint64_t off)
{
	return ftruncate(bc->fd, (off_t)off) == 0 ? 0 : -1;
}

static int
__recover(struct bitcask *bc)
{
	off_t fsize = lseek(bc->fd, 0, SEEK_END);
	uint64_t off = 0;
	uint8_t  hdr[HDR_SIZE];
	uint8_t *body = NULL;
	size_t   body_cap = 0;
	int      rc = 0;
	int64_t  now = wall_ns();

	if (fsize < 0) return -1;

	while ((uint64_t)fsize - off >= HDR_SIZE) {
		ssize_t n = pread(bc->fd, hdr, HDR_SIZE, (off_t)off);
		if (n != (ssize_t)HDR_SIZE) {
			/* Trailing garbage; truncate. */
			rc = __cut_tail(bc, off);
			break;
		}
		uint32_t crc_stored = get_u32(hdr);
		uint64_t ts         = get_u64(hdr + 4);
		uint32_t kl         = get_u32(hdr + 12);
		uint32_t vl         = get_u32(hdr + 16);
		uint32_t body_len;
		if (__rec_parse(kl, vl, &body_len) != 0) {
			rc = __cut_tail(bc, off);
			break;
		}
		if ((uint64_t)fsize - off - HDR_SIZE < body_len) {
			rc = __cut_tail(bc, off);
			break;
		}
		if (body_len > body_cap) {
			void *nb = realloc(body, body_len);
			if (nb == NULL) { rc = -1; break; }
			body = nb;
			body_cap = body_len;
		}
		if (body_len > 0) {
			n = pread(bc->fd, body, body_len,
			    (off_t)(off + HDR_SIZE));
			if (n != (ssize_t)body_len) {
				rc = __cut_tail(bc, off);
				break;
			}
		}
		/* Verify CRC. */
		{
			uint32_t crc_calc;
			uint8_t hdr_for_crc[HDR_SIZE - 4];
			memcpy(hdr_for_crc, hdr + 4, HDR_SIZE - 4);
			crc_calc = crc32(hdr_for_crc, HDR_SIZE - 4, 0);
			crc_calc = crc32(body, body_len, crc_calc);
			if (crc_calc != crc_stored) {
				/* Corrupt record at this offset; truncate
				 * and stop -- subsequent records may also
				 * be corrupt. */
				rc = __cut_tail(bc, off);
				break;
			}
		}
		if (vl != REC_BATCH) {
			__rec_apply(bc, off, ts, kl, vl, body, now);
		} else if (__batch_apply(bc, off + HDR_SIZE, body, body_len,
		    now) != 0) {
			fprintf(stderr, "bitcask: malformed batch record at "
			    "offset %llu\n", (unsigned long long)off);
			rc = -1;
			break;
		}
		off += HDR_SIZE + body_len;
	}
	bc->write_offset = off;
	free(body);
	return rc;
}

/* ---- public API --------------------------------------------- */

/*
 * A directory from before the v2 records holds DIR/bitcask.data.  Its
 * records are valid v2 log content, so rename it to `path` (the v2
 * name) and fsync the directory so the rename is durable.  An older
 * binary that meets a v2 log then sees no file of its own and starts
 * empty, rather than truncating the log at the first v2 record.
 * Both files present means an older binary ran on a v2 directory and
 * wrote its own log: neither may be dropped silently, so refuse.
 * Returns 0, or -1 (refused, or the rename failed).
 */
static int
__adopt_v1(const char *dir, const char *path)
{
	char v1[512];
	int dfd, have_v2;

	snprintf(v1, sizeof v1, "%s/bitcask.data", dir);
	have_v2 = access(path, F_OK) == 0;
	if (access(v1, F_OK) != 0)
		return 0;
	if (have_v2) {
		fprintf(stderr, "bitcask: %s and %s both exist (an older "
		    "binary wrote the first); move one away\n", v1, path);
		return -1;
	}
	if (rename(v1, path) != 0)
		return -1;
	if ((dfd = open(dir, O_RDONLY)) >= 0) {
		(void)fsync(dfd);
		(void)close(dfd);
	}
	return 0;
}

int
bitcask_open(const char *dir, bitcask_t **out)
{
	bitcask_t *bc;
	char path[512];
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
	if (__adopt_v1(dir, path) != 0) {
		lh_destroy(&bc->idx, __idx_mfree, NULL);
		free(bc);
		return -1;
	}
	bc->fd = open(path, O_RDWR | O_CREAT, 0644);
	if (bc->fd < 0) {
		lh_destroy(&bc->idx, __idx_mfree, NULL);
		free(bc);
		return -1;
	}
	if (__recover(bc) != 0) {
		(void)close(bc->fd); __idx_free_all(bc); free(bc); return -1;
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
	uint8_t hdr_for_crc[HDR_SIZE - 4];
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

	put_u64(hdr_for_crc + 0, ts);
	put_u32(hdr_for_crc + 8, (uint32_t)key_len);
	put_u32(hdr_for_crc + 12, vl);
	crc = crc32(hdr_for_crc, sizeof hdr_for_crc, 0);
	crc = crc32(key, key_len, crc);
	crc = crc32(meta, meta_len, crc);
	crc = crc32(val, val_len, crc);
	put_u32(hdr, crc);
	memcpy(hdr + 4, hdr_for_crc, sizeof hdr_for_crc);

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
 * len, the inner records).
 */
static void
__marker_hdr(uint8_t *hdr, const void *body, size_t len, uint32_t kl,
    uint32_t kind)
{
	uint8_t hdr_for_crc[HDR_SIZE - 4];
	uint32_t crc;

	put_u64(hdr_for_crc + 0, (uint64_t)now_ns());
	put_u32(hdr_for_crc + 8, kl);
	put_u32(hdr_for_crc + 12, kind);
	crc = crc32(hdr_for_crc, sizeof hdr_for_crc, 0);
	crc = crc32(body, len, crc);
	put_u32(hdr, crc);
	memcpy(hdr + 4, hdr_for_crc, sizeof hdr_for_crc);
}

/* Append a record with no value bytes: a tombstone (key) or a flush
 * (no key).  Returns 0 or -1. */
static int
__rec_marker(struct bitcask *bc, const void *key, size_t key_len,
    uint32_t kind)
{
	uint8_t hdr[HDR_SIZE];
	struct iovec iov[2];

	__marker_hdr(hdr, key, key_len, (uint32_t)key_len, kind);
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
		    TOMBSTONE);
		memcpy(p + HDR_SIZE, keys[i], lens[i]);
		p += HDR_SIZE + lens[i];
	}
	__marker_hdr(buf, buf + HDR_SIZE, inner, (uint32_t)inner, REC_BATCH);
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
	uint8_t hdr[HDR_SIZE], hdr_for_crc[HDR_SIZE - 4], meta[META_SIZE];
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
	put_u64(hdr_for_crc + 0, e->timestamp);
	put_u32(hdr_for_crc + 8, (uint32_t)e->key_len);
	put_u32(hdr_for_crc + 12, vl);
	crc = crc32(hdr_for_crc, sizeof hdr_for_crc, 0);
	crc = crc32(e->key, e->key_len, crc);
	crc = crc32(meta, meta_len, crc);
	crc = crc32(val, e->val_len, crc);
	put_u32(hdr, crc);
	memcpy(hdr + 4, hdr_for_crc, sizeof hdr_for_crc);

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
 * fsync'd, then renamed over the data file; the in-memory index
 * offsets are re-pointed into the new file as each record is written.
 * Holds the write lock for the whole pass (a single-file Bitcask
 * cannot serve reads from the old file while the new one is being
 * built); a multi-file variant would merge immutable segments in the
 * background instead.
 */
int
bitcask_compact(bitcask_t *bc)
{
	char tmp_path[512], data_path[512];
	int tfd;
	uint64_t new_off = 0;
	int64_t now = wall_ns();
	size_t i;
	int rc = -1;

	if (bc == NULL) return -1;

	snprintf(tmp_path, sizeof tmp_path, "%s/bitcask.compact", bc->dir);
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

	/* Swap to the compacted file. */
	(void)close(bc->fd);
	bc->fd = tfd;
	bc->write_offset = new_off;
	bc->bytes_used = new_off;
	bc->bytes_dead = 0;
	rc = 0;
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
