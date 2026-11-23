/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test_bitcask.c -- Bitcask KV unit tests.
 */

#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "bitcask.h"
#include "t_assert.h"
#include "xtc.h"


static char *
mkdir_temp(void)
{
	static char dir[256];
	const char *base = getenv("TMPDIR");
	snprintf(dir, sizeof dir, "%s/bitcask-test-%d",
	    base != NULL && *base != '\0' ? base : "/tmp", (int)getpid());
	(void)rmdir(dir);
	if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
		perror("mkdir");
		exit(1);
	}
	return dir;
}

static void
rmrf(const char *dir)
{
	char path[512];
	snprintf(path, sizeof path, "%s/bitcask.data", dir);
	(void)unlink(path);
	snprintf(path, sizeof path, "%s/bitcask.v2", dir);
	(void)unlink(path);
	(void)rmdir(dir);
}

static void
test_basic_put_get(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	char buf[64];
	size_t n;

	ASSERT(bitcask_open(dir, &bc) == 0);
	ASSERT(bitcask_put(bc, "k1", 2, "value1", 6) == 0);
	ASSERT(bitcask_get(bc, "k1", 2, buf, sizeof buf, &n) == 0);
	ASSERT(n == 6);
	ASSERT(memcmp(buf, "value1", 6) == 0);
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   basic_put_get\n");
}

static void
test_overwrite(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	char buf[64];
	size_t n;
	ASSERT(bitcask_open(dir, &bc) == 0);
	ASSERT(bitcask_put(bc, "k", 1, "first", 5) == 0);
	ASSERT(bitcask_put(bc, "k", 1, "second", 6) == 0);
	ASSERT(bitcask_get(bc, "k", 1, buf, sizeof buf, &n) == 0);
	ASSERT(n == 6);
	ASSERT(memcmp(buf, "second", 6) == 0);
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   overwrite\n");
}

static void
test_delete(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	char buf[16];
	size_t n;
	ASSERT(bitcask_open(dir, &bc) == 0);
	ASSERT(bitcask_put(bc, "k", 1, "v", 1) == 0);
	ASSERT(bitcask_del(bc, "k", 1) == 0);
	ASSERT(bitcask_get(bc, "k", 1, buf, sizeof buf, &n) == 1);
	ASSERT(bitcask_del(bc, "k", 1) == 1);   /* second del = miss */
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   delete\n");
}

static void
test_recovery(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	char buf[64];
	size_t n;
	int i;

	/* Phase 1: insert 100 keys, close. */
	ASSERT(bitcask_open(dir, &bc) == 0);
	for (i = 0; i < 100; i++) {
		char k[16], v[32];
		snprintf(k, sizeof k, "key%d", i);
		snprintf(v, sizeof v, "value-%d", i);
		ASSERT(bitcask_put(bc, k, strlen(k), v, strlen(v)) == 0);
	}
	bitcask_close(bc);

	/* Phase 2: re-open, verify all 100 keys come back. */
	ASSERT(bitcask_open(dir, &bc) == 0);
	for (i = 0; i < 100; i++) {
		char k[16], expect[32];
		snprintf(k, sizeof k, "key%d", i);
		snprintf(expect, sizeof expect, "value-%d", i);
		ASSERT(bitcask_get(bc, k, strlen(k), buf, sizeof buf, &n) == 0);
		ASSERT(n == strlen(expect));
		ASSERT(memcmp(buf, expect, n) == 0);
	}
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   recovery\n");
}

static void
test_recovery_with_deletes_and_overwrites(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	char buf[64];
	size_t n;

	ASSERT(bitcask_open(dir, &bc) == 0);
	bitcask_put(bc, "alpha",  5, "1", 1);
	bitcask_put(bc, "alpha",  5, "2", 1);    /* overwrite */
	bitcask_put(bc, "alpha",  5, "3", 1);    /* overwrite */
	bitcask_put(bc, "beta",   4, "B", 1);
	bitcask_put(bc, "gamma",  5, "G", 1);
	bitcask_del(bc, "beta",   4);             /* delete */
	bitcask_put(bc, "gamma",  5, "G2", 2);    /* overwrite */
	bitcask_close(bc);

	/* Recover. */
	ASSERT(bitcask_open(dir, &bc) == 0);
	ASSERT(bitcask_get(bc, "alpha", 5, buf, sizeof buf, &n) == 0);
	ASSERT(n == 1 && buf[0] == '3');
	ASSERT(bitcask_get(bc, "beta",  4, buf, sizeof buf, &n) == 1);   /* deleted */
	ASSERT(bitcask_get(bc, "gamma", 5, buf, sizeof buf, &n) == 0);
	ASSERT(n == 2 && memcmp(buf, "G2", 2) == 0);
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   recovery_with_deletes_and_overwrites\n");
}

static int
__count_iter(const void *k, size_t kl, void *u)
{
	(void)k; (void)kl;
	(*(int *)u)++;
	return 0;
}

static void
test_iterate_and_stats(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	bitcask_stats_t s;
	int count = 0;

	ASSERT(bitcask_open(dir, &bc) == 0);
	bitcask_put(bc, "a", 1, "1", 1);
	bitcask_put(bc, "b", 1, "2", 1);
	bitcask_put(bc, "c", 1, "3", 1);
	bitcask_del(bc, "b", 1);

	bitcask_iterate(bc, __count_iter, &count);
	ASSERT(count == 2);

	bitcask_stat(bc, &s);
	ASSERT(s.n_keys == 2);
	ASSERT(s.puts == 3);
	ASSERT(s.dels == 1);
	bitcask_close(bc);

	/* After a reopen the log's bytes are all counted as used, and the
	 * superseded ones (b's put and its tombstone) also as dead. */
	ASSERT(bitcask_open(dir, &bc) == 0);
	bitcask_stat(bc, &s);
	ASSERT(s.bytes_used > s.bytes_dead && s.bytes_dead > 0);
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   iterate_and_stats\n");
}


static void
test_compaction(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	bitcask_stats_t st;
	char key[32], buf[64];
	size_t n;
	int i;

	ASSERT(bitcask_open(dir, &bc) == 0);

	/* 200 keys, each overwritten 5x (lots of dead bytes), then a
	 * quarter deleted. */
	for (i = 0; i < 200; i++) {
		int r;
		snprintf(key, sizeof key, "key-%03d", i);
		for (r = 0; r < 5; r++) {
			char val[32];
			snprintf(val, sizeof val, "val-%03d-rev-%d", i, r);
			ASSERT(bitcask_put(bc, key, strlen(key),
			    val, strlen(val)) == 0);
		}
	}
	for (i = 0; i < 200; i += 4) {
		snprintf(key, sizeof key, "key-%03d", i);
		ASSERT(bitcask_del(bc, key, strlen(key)) == 0);
	}

	bitcask_stat(bc, &st);
	ASSERT(st.bytes_dead > 0);          /* garbage accumulated */

	ASSERT(bitcask_compact(bc) == 0);

	bitcask_stat(bc, &st);
	ASSERT(st.bytes_dead == 0);          /* reclaimed */

	/* Every surviving key reads back its last revision; deleted keys
	 * stay gone -- across compaction. */
	for (i = 0; i < 200; i++) {
		char want[32];
		snprintf(key, sizeof key, "key-%03d", i);
		snprintf(want, sizeof want, "val-%03d-rev-4", i);
		if (i % 4 == 0) {
			ASSERT(bitcask_get(bc, key, strlen(key), buf,
			    sizeof buf, &n) != 0);   /* deleted */
		} else {
			ASSERT(bitcask_get(bc, key, strlen(key), buf,
			    sizeof buf, &n) == 0);
			ASSERT(n == strlen(want));
			ASSERT(memcmp(buf, want, n) == 0);
		}
	}

	/* The compacted file survives a reopen (recovery from the merged
	 * data file). */
	bitcask_close(bc);
	ASSERT(bitcask_open(dir, &bc) == 0);
	snprintf(key, sizeof key, "key-077");
	ASSERT(bitcask_get(bc, key, strlen(key), buf, sizeof buf, &n) == 0);
	ASSERT(memcmp(buf, "val-077-rev-4", n) == 0);
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   compaction\n");
}

/*
 * Meta, flush and expiry, across a reopen and across a merge: a flush
 * makes every earlier record dead (a merge must not bring a flushed key
 * back), a key with a future expiry keeps its tag and expiry through
 * both, and a key whose expiry has passed is dropped by both.
 */
static void
test_meta_flush_expiry(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	bitcask_meta_t m, live = { 7, 0 }, gone = { 0, 0 };
	bitcask_stats_t st;
	char buf[16];
	size_t n;
	int pass;

	live.expire_at = xtc_clock_real() + 3600LL * 1000000000LL;
	ASSERT(bitcask_open(dir, &bc) == 0);
	ASSERT(bitcask_put(bc, "old1", 4, "x", 1) == 0);
	ASSERT(bitcask_put(bc, "old2", 4, "y", 1) == 0);
	ASSERT(bitcask_flush(bc) == 0);
	ASSERT(bitcask_get(bc, "old1", 4, buf, sizeof buf, &n) == 1);
	ASSERT(bitcask_put_meta(bc, "ttl", 3, "t", 1, &live) == 0);
	ASSERT(bitcask_put(bc, "plain", 5, "", 0) == 0);  /* empty value */
	/* Expires 50 ms from now: alive at the put, gone by the checks. */
	gone.expire_at = xtc_clock_real() + 50LL * 1000000LL;
	ASSERT(bitcask_put_meta(bc, "short", 5, "s", 1, &gone) == 0);
	bitcask_close(bc);
	usleep(100 * 1000);

	/* Pass 0 checks the reopened log, pass 1 the merged one, pass 2 a
	 * reopen of the merged one. */
	for (pass = 0; pass < 3; pass++) {
		ASSERT(bitcask_open(dir, &bc) == 0);
		if (pass == 1) {
			ASSERT(bitcask_compact(bc) == 0);
			bitcask_stat(bc, &st);
			ASSERT(st.bytes_dead == 0);
		}
		ASSERT(bitcask_get(bc, "old1", 4, buf, sizeof buf, &n) == 1);
		ASSERT(bitcask_get(bc, "old2", 4, buf, sizeof buf, &n) == 1);
		ASSERT(bitcask_get(bc, "short", 5, buf, sizeof buf, &n) == 1);
		ASSERT(bitcask_get(bc, "ttl", 3, buf, sizeof buf, &n) == 0);
		ASSERT(n == 1 && buf[0] == 't');
		ASSERT(bitcask_meta(bc, "ttl", 3, &m) == 0);
		ASSERT(m.tag == 7 && m.expire_at == live.expire_at);
		ASSERT(bitcask_get(bc, "plain", 5, buf, sizeof buf, &n) == 0);
		ASSERT(n == 0);
		ASSERT(bitcask_meta(bc, "plain", 5, &m) == 0);
		ASSERT(m.tag == 0 && m.expire_at == 0);
		bitcask_stat(bc, &st);
		ASSERT(st.n_keys == 2);
		bitcask_close(bc);
	}
	rmrf(dir);
	printf("  ok   meta_flush_expiry\n");
}

/* CRC32 (IEEE), as the older rexis wrote it, for the v1 records. */
static uint32_t
v1_crc(const uint8_t *p, size_t len, uint32_t c)
{
	size_t i;
	int k;

	c = ~c;
	for (i = 0; i < len; i++) {
		c ^= p[i];
		for (k = 0; k < 8; k++)
			c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
	}
	return ~c;
}

static void
be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/*
 * Append a record in the older rexis's format to the file at `path`:
 * a 20-byte header (crc, timestamp, key_len, val_len) and the key and
 * value; val NULL writes a tombstone.
 */
static void
v1_append(const char *path, const char *key, const char *val)
{
	uint8_t rec[128];
	size_t kl = strlen(key), vl = val != NULL ? strlen(val) : 0;
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);

	ASSERT(fd >= 0 && 20 + kl + vl <= sizeof rec);
	memset(rec + 4, 0, 8);
	rec[11] = 1;                         /* timestamp 1 */
	be32(rec + 12, (uint32_t)kl);
	be32(rec + 16, val != NULL ? (uint32_t)vl : 0xffffffffu);
	memcpy(rec + 20, key, kl);
	memcpy(rec + 20 + kl, val != NULL ? val : "", vl);
	be32(rec, v1_crc(rec + 4, 16 + kl + vl, 0));
	ASSERT(write(fd, rec, 20 + kl + vl) == (ssize_t)(20 + kl + vl));
	ASSERT(close(fd) == 0);
}

/* Copy file `from` to `to`, its first `len` bytes (all if -1). */
static void
copy_prefix(const char *from, const char *to, long len)
{
	char buf[4096];
	ssize_t n;
	int in = open(from, O_RDONLY);
	int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	ASSERT(in >= 0 && out >= 0);
	while ((n = read(in, buf, sizeof buf)) > 0) {
		if (len >= 0 && n > len)
			n = (ssize_t)len;
		ASSERT(write(out, buf, (size_t)n) == n);
		if (len >= 0 && (len -= n) == 0)
			break;
	}
	ASSERT(close(in) == 0 && close(out) == 0);
}

/* The keyspace of the older log written in test_adopt_v1_log. */
static void
assert_v1_keys(const char *dir)
{
	bitcask_t *bc;
	char buf[16];
	size_t n;

	ASSERT(bitcask_open(dir, &bc) == 0);
	ASSERT(bitcask_get(bc, "k", 1, buf, sizeof buf, &n) == 0);
	ASSERT(n == 2 && memcmp(buf, "v2", 2) == 0);
	ASSERT(bitcask_get(bc, "gone", 4, buf, sizeof buf, &n) == 1);
	ASSERT(bitcask_get(bc, "x", 1, buf, sizeof buf, &n) == 0);
	ASSERT(n == 3 && memcmp(buf, "xyz", 3) == 0);
	ASSERT(bitcask_get(bc, "e", 1, buf, sizeof buf, &n) == 0 && n == 0);
	bitcask_close(bc);
}

/*
 * A directory written by an older rexis (DIR/bitcask.data, 20-byte
 * headers) is converted: renamed to bitcask.v1, merged into bitcask.v2
 * (a temp file renamed in), and bitcask.v1 removed.  A crash after any
 * step leaves one of the states below on disk; each must open with the
 * older log's keyspace.  Then: an empty bitcask.data next to bitcask.v2
 * is removed, and one with records, or bitcask.data next to bitcask.v1,
 * is refused with both files kept.
 */
static void
test_adopt_v1_log(void)
{
	const char *dir = mkdir_temp();
	char data[512], v1[512], v2[512], tmp[512], orig[512], conv[512];
	bitcask_t *bc;
	int fd;

	snprintf(data, sizeof data, "%s/bitcask.data", dir);
	snprintf(v1, sizeof v1, "%s/bitcask.v1", dir);
	snprintf(v2, sizeof v2, "%s/bitcask.v2", dir);
	snprintf(tmp, sizeof tmp, "%s/bitcask.compact", dir);
	snprintf(orig, sizeof orig, "%s/orig.data", dir);
	snprintf(conv, sizeof conv, "%s/conv.v2", dir);
	v1_append(orig, "k", "v1");
	v1_append(orig, "gone", "g");
	v1_append(orig, "x", "xyz");
	v1_append(orig, "k", "v2");
	v1_append(orig, "gone", NULL);
	v1_append(orig, "e", "");

	/* Before the v1 rename: bitcask.data alone. */
	copy_prefix(orig, data, -1);
	assert_v1_keys(dir);
	ASSERT(access(data, F_OK) != 0 && access(v1, F_OK) != 0);
	ASSERT(access(tmp, F_OK) != 0);
	copy_prefix(v2, conv, -1);            /* a whole conversion */

	/* After the v1 removal: bitcask.v2 alone. */
	assert_v1_keys(dir);

	/* After the v1 rename: bitcask.v1 alone. */
	ASSERT(unlink(v2) == 0);
	copy_prefix(orig, v1, -1);
	assert_v1_keys(dir);
	ASSERT(access(v1, F_OK) != 0);

	/* In the middle of the merge: bitcask.v1 and part of the temp. */
	ASSERT(unlink(v2) == 0);
	copy_prefix(orig, v1, -1);
	copy_prefix(conv, tmp, 30);
	assert_v1_keys(dir);
	ASSERT(access(v1, F_OK) != 0 && access(tmp, F_OK) != 0);

	/* After the v2 rename: bitcask.v1 and a whole bitcask.v2. */
	copy_prefix(orig, v1, -1);
	assert_v1_keys(dir);
	ASSERT(access(v1, F_OK) != 0);

	/* bitcask.data next to bitcask.v1: no step leaves both. */
	copy_prefix(orig, v1, -1);
	copy_prefix(orig, data, -1);
	ASSERT(bitcask_open(dir, &bc) == -1);
	ASSERT(access(data, F_OK) == 0 && access(v1, F_OK) == 0);
	ASSERT(unlink(v1) == 0 && unlink(data) == 0);

	/* An older binary run on the converted directory leaves an empty
	 * bitcask.data: removed, and the open goes on. */
	ASSERT(close(open(data, O_CREAT | O_WRONLY, 0644)) == 0);
	assert_v1_keys(dir);
	ASSERT(access(data, F_OK) != 0);

	/* One with records is refused, and both files are kept. */
	fd = open(data, O_CREAT | O_WRONLY, 0644);
	ASSERT(fd >= 0 && write(fd, "x", 1) == 1 && close(fd) == 0);
	ASSERT(bitcask_open(dir, &bc) == -1);
	ASSERT(access(data, F_OK) == 0 && access(v2, F_OK) == 0);
	(void)unlink(data);
	(void)unlink(orig);
	(void)unlink(conv);
	rmrf(dir);
	printf("  ok   adopt_v1_log\n");
}

/* Flip one byte of the file at `off`. */
static void
flip_byte(const char *path, off_t off)
{
	int fd = open(path, O_RDWR);
	unsigned char c;

	ASSERT(fd >= 0);
	ASSERT(pread(fd, &c, 1, off) == 1);
	c ^= 0xff;
	ASSERT(pwrite(fd, &c, 1, off) == 1);
	ASSERT(close(fd) == 0);
}

static off_t
file_size(const char *path)
{
	struct stat sb;

	ASSERT(stat(path, &sb) == 0);
	return sb.st_size;
}

/* The whole of `path`, in a malloc'd buffer; *len is its size. */
static uint8_t *
slurp(const char *path, off_t *len)
{
	uint8_t *b;
	int fd;

	*len = file_size(path);
	b = malloc((size_t)*len + 1);
	fd = open(path, O_RDONLY);
	ASSERT(b != NULL && fd >= 0);
	ASSERT(read(fd, b, (size_t)*len) == *len && close(fd) == 0);
	return b;
}

/*
 * Flip each bit of an older log (bitcask.data) and convert it.  An older
 * header has no CRC, so a flipped length can make a record seem to run
 * past the end of the file.  Each flip must refuse the open, leaving
 * bitcask.v1 as it was (and no bitcask.v2), or, for a flip in the last
 * record, convert everything before it.
 */
static void
test_v1_flip_sweep(void)
{
	const char *dir = mkdir_temp();
	char data[512], v1[512], v2[512];
	uint8_t *img, *got;
	bitcask_t *bc;
	char buf[16];
	off_t size, last, len;
	size_t n;
	int i, bit, fd, refused = 0, cut = 0;

	snprintf(data, sizeof data, "%s/bitcask.data", dir);
	snprintf(v1, sizeof v1, "%s/bitcask.v1", dir);
	snprintf(v2, sizeof v2, "%s/bitcask.v2", dir);
	v1_append(data, "k", "v1");
	v1_append(data, "gone", "g");
	v1_append(data, "x", "xyz");
	v1_append(data, "gone", NULL);
	last = file_size(data);
	v1_append(data, "k", "v2");
	img = slurp(data, &size);
	ASSERT(unlink(data) == 0);

	for (i = 0; i < size * 8; i++) {
		bit = 1 << (i % 8);
		img[i / 8] ^= (uint8_t)bit;
		fd = open(data, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		ASSERT(fd >= 0 && write(fd, img, (size_t)size) == size);
		ASSERT(close(fd) == 0);
		if (bitcask_open(dir, &bc) != 0) {
			ASSERT(i / 8 < last && access(v2, F_OK) != 0);
			got = slurp(v1, &len);
			ASSERT(len == size && memcmp(got, img, (size_t)len) == 0);
			free(got);
			ASSERT(unlink(v1) == 0);
			refused++;
		} else {
			ASSERT(i / 8 >= last && access(v1, F_OK) != 0);
			ASSERT(bitcask_get(bc, "k", 1, buf, sizeof buf,
			    &n) == 0);
			ASSERT(n == 2 && memcmp(buf, "v1", 2) == 0);
			ASSERT(bitcask_get(bc, "x", 1, buf, sizeof buf,
			    &n) == 0);
			bitcask_close(bc);
			ASSERT(unlink(v2) == 0);
			cut++;
		}
		img[i / 8] ^= (uint8_t)bit;
	}
	free(img);
	rmrf(dir);
	printf("  ok   v1_flip_sweep (%d flips: %d refused, %d cut at the "
	    "last record)\n", refused + cut, refused, cut);
}

/*
 * A crash tears only the last record, which recovery cuts off.  A bad
 * record with valid records after it is damage, not a crash: the open
 * is refused and the file is left as it was.  Also checks that a batch
 * of deletes (bitcask_del_many) survives a reopen, and that a batch
 * torn at the tail is cut off whole.
 */
static void
test_corrupt_vs_torn(void)
{
	const char *dir = mkdir_temp();
	const void *keys[3] = { "a", "b", "c" };
	const size_t lens[3] = { 1, 1, 1 };
	char path[512], key[16], buf[16];
	bitcask_t *bc;
	off_t mid, size;
	size_t n;
	int i;

	snprintf(path, sizeof path, "%s/bitcask.v2", dir);
	ASSERT(bitcask_open(dir, &bc) == 0);
	for (i = 0; i < 20; i++) {
		snprintf(key, sizeof key, "key-%02d", i);
		ASSERT(bitcask_put(bc, key, strlen(key), "value", 5) == 0);
	}
	bitcask_close(bc);

	/* A byte flipped in the 10th record's value: refused, untouched. */
	size = file_size(path);
	mid = size / 2;
	flip_byte(path, mid);
	ASSERT(bitcask_open(dir, &bc) == -1);
	ASSERT(file_size(path) == size);
	flip_byte(path, mid);

	/* The same flip in the last record: a torn tail, cut off. */
	flip_byte(path, size - 1);
	ASSERT(bitcask_open(dir, &bc) == 0);
	ASSERT(bitcask_get(bc, "key-19", 6, buf, sizeof buf, &n) == 1);
	ASSERT(bitcask_get(bc, "key-18", 6, buf, sizeof buf, &n) == 0);

	/* A batch of deletes, reopened; then one torn at the tail. */
	ASSERT(bitcask_put(bc, "a", 1, "1", 1) == 0);
	ASSERT(bitcask_put(bc, "b", 1, "2", 1) == 0);
	ASSERT(bitcask_put(bc, "c", 1, "3", 1) == 0);
	size = file_size(path);
	ASSERT(bitcask_del_many(bc, keys, lens, 2) == 0);
	bitcask_close(bc);
	ASSERT(bitcask_open(dir, &bc) == 0);
	ASSERT(bitcask_get(bc, "a", 1, buf, sizeof buf, &n) == 1);
	ASSERT(bitcask_get(bc, "b", 1, buf, sizeof buf, &n) == 1);
	ASSERT(bitcask_get(bc, "c", 1, buf, sizeof buf, &n) == 0);
	ASSERT(bitcask_put(bc, "a", 1, "1", 1) == 0);
	size = file_size(path);
	ASSERT(bitcask_del_many(bc, keys, lens, 3) == 0);
	bitcask_close(bc);
	ASSERT(truncate(path, file_size(path) - 3) == 0);
	ASSERT(bitcask_open(dir, &bc) == 0);
	ASSERT(file_size(path) == size);
	ASSERT(bitcask_get(bc, "a", 1, buf, sizeof buf, &n) == 0);
	ASSERT(bitcask_get(bc, "c", 1, buf, sizeof buf, &n) == 0);
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   corrupt_vs_torn\n");
}

/*
 * Flip one bit of every byte of a log (bit i % 8 of byte i) and open
 * it.  Each flip must refuse the open and leave the file as it was, or,
 * for a flip in the last record, cut the log back to exactly the
 * record before it.  So no flip drops an acknowledged record silently:
 * a damaged length no longer passes for a torn tail (the header CRC),
 * and a damaged batch's inner records never pass for records of the
 * log (their own CRC seed), so a batch as the last record is cut.
 */
static void
test_flip_sweep(void)
{
	const char *dir = mkdir_temp();
	const void *keys[3] = { "k1", "k2", "k3" };
	const size_t lens[3] = { 2, 2, 2 };
	bitcask_meta_t m = { 2, 0 };
	char path[512], key[16];
	uint8_t *img;
	bitcask_t *bc;
	off_t size, last, batch;
	int i, fd, refused = 0, cut = 0;

	snprintf(path, sizeof path, "%s/bitcask.v2", dir);
	ASSERT(bitcask_open(dir, &bc) == 0);
	for (i = 0; i < 6; i++) {
		snprintf(key, sizeof key, "k%d", i);
		ASSERT(bitcask_put(bc, key, 2, "vvvvvvvv", 8) == 0);
	}
	ASSERT(bitcask_put_meta(bc, "l", 1, "listbody", 8, &m) == 0);
	batch = file_size(path);
	ASSERT(bitcask_del_many(bc, keys, lens, 3) == 0);
	last = file_size(path);
	ASSERT(bitcask_put(bc, "last", 4, "xxxxxxxxxx", 10) == 0);
	bitcask_close(bc);
	size = file_size(path);
	img = malloc((size_t)size);
	fd = open(path, O_RDONLY);
	ASSERT(img != NULL && fd >= 0);
	ASSERT(read(fd, img, (size_t)size) == size && close(fd) == 0);

	for (i = 0; i < size; i++) {
		img[i] ^= (uint8_t)(1u << (i % 8));
		fd = open(path, O_WRONLY | O_TRUNC);
		ASSERT(fd >= 0 && write(fd, img, (size_t)size) == size);
		ASSERT(close(fd) == 0);
		img[i] ^= (uint8_t)(1u << (i % 8));
		if (bitcask_open(dir, &bc) != 0) {
			ASSERT(i < last && file_size(path) == size);
			refused++;
			continue;
		}
		bitcask_close(bc);
		ASSERT(i >= last && file_size(path) == last);
		cut++;
	}

	/* The batch as the last record, its CRC flipped: cut, not refused
	 * for the valid-looking tombstones inside it. */
	fd = open(path, O_WRONLY | O_TRUNC);
	img[batch + 4] ^= 1;
	ASSERT(fd >= 0 && write(fd, img, (size_t)last) == last);
	ASSERT(close(fd) == 0);
	ASSERT(bitcask_open(dir, &bc) == 0);
	bitcask_close(bc);
	ASSERT(file_size(path) == batch);
	free(img);
	rmrf(dir);
	printf("  ok   flip_sweep (%d flips: %d refused, %d cut at the last "
	    "record)\n", refused + cut, refused, cut);
}

/*
 * A bad record in the middle of the log, and the search for a valid
 * record after it fails (its allocation, then its read): the open must
 * fail and leave the file byte for byte as it was, never cut it as if
 * nothing followed.
 */
static void
test_scan_fault(void)
{
	const char *dir = mkdir_temp();
	char path[512], key[16];
	uint8_t *before, *after;
	bitcask_t *bc;
	off_t rec1, size, len;
	int i, at;

	snprintf(path, sizeof path, "%s/bitcask.v2", dir);
	ASSERT(bitcask_open(dir, &bc) == 0);
	ASSERT(bitcask_put(bc, "k0", 2, "v", 1) == 0);
	rec1 = file_size(path);
	for (i = 1; i <= 20; i++) {
		snprintf(key, sizeof key, "k%d", i);
		ASSERT(bitcask_put(bc, key, strlen(key), "v", 1) == 0);
	}
	bitcask_close(bc);
	flip_byte(path, rec1 + 24);          /* record 1's key */
	before = slurp(path, &size);

	for (at = 1; at <= 2; at++) {
		bitcask_scan_fail_at = at;
		ASSERT(bitcask_open(dir, &bc) == -1);
		ASSERT(bitcask_scan_fail_at == 0);
		after = slurp(path, &len);
		ASSERT(len == size && memcmp(before, after, (size_t)len) == 0);
		free(after);
	}
	ASSERT(bitcask_open(dir, &bc) == -1);   /* refused for the damage */
	free(before);
	rmrf(dir);
	printf("  ok   scan_fault\n");
}

/*
 * The in-memory index must grow with the key count.  A fixed 4,096-bucket
 * chained index walked ~N/4096 entries on EVERY put (to find the old
 * record), so a 5M-key load fell from 254k to 7.6k puts/s.  Time a fixed
 * batch of overwrites at 4k keys and at 400k keys; the index must keep
 * it within 8x (the working set is 100x larger, so some cache misses are
 * expected), and every key must survive the growth and a reopen.
 */
static void
test_index_grows(void)
{
	const char *dir = mkdir_temp();
	bitcask_t *bc;
	bitcask_stats_t st;
	char key[32], buf[64];
	size_t n;
	int i, r, kl;
	int64_t t0, small_ns, big_ns;

	ASSERT(bitcask_open(dir, &bc) == 0);
	for (i = 0; i < 4000; i++) {
		kl = snprintf(key, sizeof key, "ik-%d", i);
		ASSERT(bitcask_put(bc, key, (size_t)kl, "v", 1) == 0);
	}
	t0 = xtc_clock_mono();
	for (r = 0; r < 10; r++)
		for (i = 0; i < 4000; i++) {
			kl = snprintf(key, sizeof key, "ik-%d", i);
			ASSERT(bitcask_size(bc, key, (size_t)kl, &n) == 0);
		}
	small_ns = xtc_clock_mono() - t0;
	for (i = 4000; i < 400000; i++) {
		kl = snprintf(key, sizeof key, "ik-%d", i);
		ASSERT(bitcask_put(bc, key, (size_t)kl, "v", 1) == 0);
	}
	t0 = xtc_clock_mono();
	for (r = 0; r < 10; r++)
		for (i = 0; i < 4000; i++) {
			kl = snprintf(key, sizeof key, "ik-%d",
			    (int)(((long)i * 97) % 400000));
			ASSERT(bitcask_size(bc, key, (size_t)kl, &n) == 0);
		}
	big_ns = xtc_clock_mono() - t0;
	printf("  40k index lookups: %.1f ms at 4k keys, %.1f ms at 400k "
	    "keys (%.1fx)\n", (double)small_ns / 1e6, (double)big_ns / 1e6,
	    (double)big_ns / (double)(small_ns > 0 ? small_ns : 1));
	ASSERT(big_ns < 8 * small_ns);
	bitcask_stat(bc, &st);
	ASSERT(st.n_keys == 400000);
	bitcask_close(bc);

	ASSERT(bitcask_open(dir, &bc) == 0);
	bitcask_stat(bc, &st);
	ASSERT(st.n_keys == 400000);
	for (i = 0; i < 400000; i += 7) {
		kl = snprintf(key, sizeof key, "ik-%d", i);
		ASSERT(bitcask_get(bc, key, (size_t)kl, buf, sizeof buf,
		    &n) == 0);
		ASSERT(n == 1 && buf[0] == 'v');
	}
	ASSERT(bitcask_compact(bc) == 0);
	kl = snprintf(key, sizeof key, "ik-%d", 399999);
	ASSERT(bitcask_get(bc, key, (size_t)kl, buf, sizeof buf, &n) == 0);
	bitcask_close(bc);
	rmrf(dir);
	printf("  ok   index_grows\n");
}

int
main(void)
{
	test_basic_put_get();
	test_overwrite();
	test_delete();
	test_recovery();
	test_recovery_with_deletes_and_overwrites();
	test_iterate_and_stats();
	test_compaction();
	test_meta_flush_expiry();
	test_adopt_v1_log();
	test_v1_flip_sweep();
	test_corrupt_vs_torn();
	test_flip_sweep();
	test_scan_fault();
	test_index_grows();
	printf("All bitcask tests passed.\n");
	return 0;
}
