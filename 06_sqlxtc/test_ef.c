/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_ef.c
 *	Elias-Fano chunks (ef.c), as properties over random rowid sets.
 *	For each of several rowid distributions -- dense (u = n), u = 16 n,
 *	u = 1024 n, random 64-bit, and sets that hold INT64_MIN and
 *	INT64_MAX -- and chunk sizes 1 to EF_CHUNK, a seeded generator
 *	builds a sorted set and a page assignment, encodes it, and checks
 *	against the arrays it came from:
 *	  - the chunk is valid and its size is ef_chunk_bytes();
 *	  - every rowid is found at its rank, on its page; get and decode
 *	    return the rowids;
 *	  - a rowid not in the set (each neighbour, and random probes) is
 *	    not found;
 *	  - the bits per rowid stay within Vigna's bound, 2 + ceil(log2(u /
 *	    n)) plus the 20-byte header, and match STORAGE_DESIGN 5.3 for
 *	    full chunks (about 2, 6 and 12 bits);
 *	  - a damaged chunk (wrong length, a cleared upper bit, a page
 *	    start out of order) is refused by ef_chunk_valid.
 *	It prints the bits per rowid and the lookup cost per distribution.
 *	The seed is fixed; TEST_EF_SEED overrides it.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ef.h"
#include "xtc.h"
#include "t_tmp.h"

#define ROUNDS 400

static uint64_t g_rng;

/* splitmix64: a seeded, portable generator (the test's only source). */
static uint64_t
rnd(void)
{
	uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

static int
cmp64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;

	return x < y ? -1 : x > y;
}

enum { D_DENSE, D_U16, D_U1024, D_RAND64, D_EDGES, D_N };
static const char *g_dname[D_N] = { "dense u=n", "u=16n", "u=1024n",
    "random 64-bit", "INT64_MIN/MAX" };
static const double g_bound[D_N] = { 2, 6, 12, 0, 0 };   /* 5.3, full chunks */

/* n distinct sorted rowids from distribution d.  Returns n. */
static uint32_t
gen(int d, uint32_t n, int64_t *rid)
{
	uint64_t gap = d == D_U16 ? 16 : d == D_U1024 ? 1024 : 1;
	int64_t at = (int64_t)(rnd() % 1000000u) - 500000;
	uint32_t i, j, k;

	if (d == D_DENSE || d == D_U16 || d == D_U1024) {
		/* Mean gap `gap`: u = gap * n. */
		for (i = 0; i < n; i++) {
			at += d == D_DENSE ? 1 : 1 + (int64_t)(rnd() %
			    (2 * gap - 1));
			rid[i] = at;
		}
		return n;
	}
	for (i = 0; i < n; i++)
		rid[i] = (int64_t)rnd();
	if (d == D_EDGES) {
		rid[0] = INT64_MIN;
		if (n > 1)
			rid[n - 1] = INT64_MAX;
		if (n > 2)
			rid[1] = INT64_MIN + 1;
	}
	qsort(rid, n, sizeof *rid, cmp64);
	for (i = 1, k = 1; i < n; i++)      /* drop duplicates, refill */
		if (rid[i] != rid[k - 1])
			rid[k++] = rid[i];
	for (j = k; j < n; j++)
		rid[j] = rid[k - 1] + (int64_t)(j - k + 1);
	if (rid[n - 1] < rid[n - 2 < n ? n - 2 : 0])
		return 0;                   /* overflowed: skip this round */
	return n;
}

/* Rows fill pages of 1..60 rows; a page step skips 0..3 pages. */
static void
gen_pages(uint32_t n, uint32_t *page)
{
	uint32_t i, left = 1 + (uint32_t)(rnd() % 60);

	page[0] = (uint32_t)(rnd() % 100000u);
	for (i = 1; i < n; i++) {
		page[i] = page[i - 1];
		if (--left == 0) {
			page[i] += 1u + (rnd() % 8u == 0 ? (uint32_t)(rnd() % 4u) : 0u);
			left = 1 + (uint32_t)(rnd() % 60);
		}
	}
}

/* Bisection over the source array: 1 if rid is in rid[0..n). */
static int
in_set(const int64_t *rid, uint32_t n, int64_t x)
{
	uint32_t lo = 0, hi = n;

	while (lo < hi) {
		uint32_t m = lo + (hi - lo) / 2;
		if (rid[m] < x)
			lo = m + 1;
		else
			hi = m;
	}
	return lo < n && rid[lo] == x;
}

static uint32_t
n_starts(const uint32_t *page, uint32_t n)
{
	uint32_t i, c = 0;

	for (i = 1; i < n; i++)
		c += page[i] != page[i - 1];
	return c;
}

/* One round: build, then check every property.  Adds the bits of the
 * EF part (no page map) to *bits for full chunks. */
static void
one_round(int d, uint32_t n, double *bits, uint32_t *nfull)
{
	static int64_t rid[EF_CHUNK], dec[EF_CHUNK];
	static uint32_t page[EF_CHUNK];
	static uint8_t buf[8192];
	uint32_t i, r, pg, np;
	size_t len;
	double u, bound, b;

	if (gen(d, n, rid) != n)
		return;
	gen_pages(n, page);
	np = n_starts(page, n);
	len = ef_chunk_build(rid, page, n, buf, sizeof buf);
	CK(len != 0 && len == ef_chunk_bytes(rid, n, np));
	CK(ef_chunk_valid(buf, len));
	if (g_fail)
		return;
	CK(ef_chunk_count(buf) == n);
	CK(ef_chunk_first(buf) == rid[0] && ef_chunk_last(buf) == rid[n - 1]);
	ef_chunk_decode(buf, dec);
	CK(memcmp(dec, rid, n * sizeof *rid) == 0);
	for (i = 0; i < n; i++) {
		CK(ef_chunk_find(buf, rid[i], &r, &pg) && r == i &&
		    pg == page[i]);
		CK(ef_chunk_get(buf, i) == rid[i]);
		if (rid[i] != INT64_MIN && !in_set(rid, n, rid[i] - 1))
			CK(!ef_chunk_find(buf, rid[i] - 1, NULL, NULL));
		if (rid[i] != INT64_MAX && !in_set(rid, n, rid[i] + 1))
			CK(!ef_chunk_find(buf, rid[i] + 1, NULL, NULL));
	}
	for (i = 0; i < 64; i++) {
		int64_t x = (int64_t)rnd();
		if (i & 1)
			x = rid[0] + (int64_t)(rnd() % (uint64_t)(rid[n - 1] -
			    rid[0] + 2 > 0 ? rid[n - 1] - rid[0] + 2 : 1));
		CK(ef_chunk_find(buf, x, NULL, NULL) == in_set(rid, n, x));
	}
	/* Vigna's bound per rowid, plus the header; u is the span. */
	b = 8.0 * (double)(len - 2u * np) / n;
	u = (double)((uint64_t)rid[n - 1] - (uint64_t)rid[0]) + 1.0;
	bound = 2.0 + ceil(log2(u / n > 1 ? u / n : 1)) + 8.0 * EF_HDR / n +
	    128.0 / n;                     /* both arrays round up to words */
	if (b > bound) {
		fprintf(stderr, "  bound: d=%d n=%u %.2f > %.2f bits\n", d, n,
		    b, bound);
		g_fail = 1;
	}
	if (n == EF_CHUNK) {
		*bits += b;
		(*nfull)++;
	}
	/* Damage: refused, never a wild read. */
	CK(!ef_chunk_valid(buf, len - 1));
	CK(!ef_chunk_valid(buf, len + 2));
	buf[EF_HDR] ^= 0x01;              /* element 0's bit is bit 0 */
	CK(!ef_chunk_valid(buf, len));
	buf[EF_HDR] ^= 0x01;
	if (np >= 2) {
		uint8_t *pm = buf + len - 2u * np, t0 = pm[0], t1 = pm[1];
		pm[0] = pm[2];
		pm[1] = pm[3];
		CK(!ef_chunk_valid(buf, len));
		pm[0] = t0;
		pm[1] = t1;
	}
	CK(ef_chunk_valid(buf, len));
}

/* ns per ef_chunk_find of a present rowid in one full chunk. */
static double
lookup_ns(int d)
{
	static int64_t rid[EF_CHUNK];
	static uint32_t page[EF_CHUNK];
	static uint8_t buf[8192];
	int64_t t0, t1;
	uint32_t i, r, sink = 0;
	const uint32_t iters = 2000000;

	while (gen(d, EF_CHUNK, rid) != EF_CHUNK)
		;
	gen_pages(EF_CHUNK, page);
	if (ef_chunk_build(rid, page, EF_CHUNK, buf, sizeof buf) == 0)
		return -1;
	t0 = xtc_clock_mono();
	for (i = 0; i < iters; i++) {
		if (ef_chunk_find(buf, rid[(i * 2654435761u) % EF_CHUNK], &r,
		    NULL))
			sink += r;
	}
	t1 = xtc_clock_mono();
	if (sink == 0xFFFFFFFFu)
		printf("%u\n", sink);
	return (double)(t1 - t0) / iters;
}

static void
t_bad_input(void)
{
	int64_t rid[3] = { 5, 5, 9 };
	uint32_t page[3] = { 0, 0, 0 };
	uint8_t buf[256];

	CK(ef_chunk_build(rid, page, 3, buf, sizeof buf) == 0);  /* not strict */
	rid[1] = 6;
	page[2] = 200;                                          /* step > 128 */
	CK(ef_chunk_build(rid, page, 3, buf, sizeof buf) == 0);
	page[2] = 1;
	CK(ef_chunk_build(rid, page, 0, buf, sizeof buf) == 0);
	CK(ef_chunk_build(rid, page, 3, buf, 10) == 0);         /* too small */
	CK(ef_chunk_build(rid, page, 3, buf, sizeof buf) != 0);
	CK(!ef_chunk_valid(buf, 3));
}

int
main(void)
{
	const char *s = getenv("TEST_EF_SEED");
	double bits[D_N] = { 0 };
	uint32_t nfull[D_N] = { 0 };
	int d, k;

	g_rng = s != NULL ? strtoull(s, NULL, 0) : 0x5EEDEF01u;
	printf("  seed 0x%llx\n", (unsigned long long)g_rng);
	t_bad_input();
	for (k = 0; k < ROUNDS && !g_fail; k++)
		for (d = 0; d < D_N; d++) {
			uint32_t n = k % 4 == 0 ? EF_CHUNK :
			    1 + (uint32_t)(rnd() % EF_CHUNK);
			if (k < 3)
				n = (uint32_t)k + 1;          /* 1, 2, 3 */
			one_round(d, n, &bits[d], &nfull[d]);
		}
	for (d = 0; d < D_N; d++) {
		double b = nfull[d] ? bits[d] / nfull[d] : 0;
		printf("  %-14s %5.2f bits/rowid (full chunks, %u), lookup "
		    "%.1f ns\n", g_dname[d], b, nfull[d], lookup_ns(d));
		/* 5.3's figures plus the header's 20 bytes per chunk, and
		 * a word of rounding in each bit array. */
		if (g_bound[d] > 0)
			CK(b <= g_bound[d] + 8.0 * EF_HDR / EF_CHUNK +
			    128.0 / EF_CHUNK);
		if (d == D_RAND64)          /* 5.3: about 2 + 64 - log2(n) */
			CK(b <= 2 + 64 - 9 + 8.0 * EF_HDR / EF_CHUNK +
			    128.0 / EF_CHUNK);
	}
	if (!g_fail)
		printf("ok test_ef (%d rounds x %d distributions)\n", ROUNDS,
		    D_N);
	return g_fail;
}
