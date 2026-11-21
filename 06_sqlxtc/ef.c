/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/ef.c
 *	Elias-Fano chunks.  See ef.h for the format.
 *
 *	The upper bits of value i (relative to the chunk's base) are
 *	high_i = v_i >> l, written as a 1 at bit high_i + i, so the zeros
 *	before element i's bit count high_i.  A chunk is small (at most
 *	EF_CHUNK values, so under 3 x EF_CHUNK upper bits, 24 words), and
 *	a lookup walks its upper words with popcount instead of keeping
 *	the forward and skip pointers Vigna adds for long lists.
 */

#include "ef.h"

#include <string.h>

#define EF_W(n) (((n) + 63u) / 64u)          /* words for n bits */

static uint64_t
ld64(const uint8_t *p)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	uint64_t v;

	memcpy(&v, p, 8);
	return v;
#else
	uint64_t v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
#endif
}

static void
st64(uint8_t *p, uint64_t v)
{
	int i;

	for (i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t
ld16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static void
st16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

/* Rowids in unsigned order: INT64_MIN maps to 0. */
static uint64_t
ef_u(int64_t rid)
{
	return (uint64_t)rid ^ (1ull << 63);
}

static int64_t
ef_s(uint64_t u)
{
	return (int64_t)(u ^ (1ull << 63));
}

/* floor(log2(span / n)), 0 when span < n. */
static uint32_t
ef_lbits(uint64_t span, uint32_t n)
{
	uint64_t q = span / n;
	uint32_t l = 0;

	while (q > 1) {
		q >>= 1;
		l++;
	}
	return l;
}

/* The decoded header of a chunk, with pointers to its three arrays. */
struct ef_view {
	uint64_t       base;
	uint32_t       n, l, uw, np, page0;
	const uint8_t *up, *lo, *pm;
};

static void
ef_view(const uint8_t *b, struct ef_view *v)
{
	v->base = ld64(b);
	v->n = ld16(b + 8);
	v->l = b[10];
	v->uw = ld16(b + 12);
	v->np = ld16(b + 14);
	v->page0 = (uint32_t)b[16] | (uint32_t)b[17] << 8 |
	    (uint32_t)b[18] << 16 | (uint32_t)b[19] << 24;
	v->up = b + EF_HDR;
	v->lo = v->up + 8u * v->uw;
	v->pm = v->lo + 8u * EF_W(v->n * v->l);
}

/* The l low bits of element i. */
static uint64_t
ef_low(const struct ef_view *v, uint32_t i)
{
	uint64_t bit, w, x;
	uint32_t sh;

	if (v->l == 0)
		return 0;
	bit = (uint64_t)i * v->l;
	w = bit / 64u;
	sh = (uint32_t)(bit % 64u);
	x = ld64(v->lo + 8u * w) >> sh;
	if (sh + v->l > 64u)
		x |= ld64(v->lo + 8u * (w + 1u)) << (64u - sh);
	return v->l == 64u ? x : x & ((1ull << v->l) - 1u);
}

size_t
ef_chunk_bytes(const int64_t *rid, uint32_t n, uint32_t np)
{
	uint64_t span;
	uint32_t l;

	if (rid == NULL || n == 0 || n > EF_CHUNK)
		return 0;
	span = ef_u(rid[n - 1]) - ef_u(rid[0]);
	l = ef_lbits(span, n);
	return EF_HDR + 8u * EF_W((uint32_t)(span >> l) + n) +
	    8u * EF_W(n * l) + 2u * np;
}

size_t
ef_chunk_build(const int64_t *rid, const uint32_t *page, uint32_t n,
    uint8_t *buf, size_t cap)
{
	uint64_t base, span, v, bit;
	uint32_t l, uw, lw, np = 0, i, step;
	uint8_t *up, *lo, *pm;
	size_t len;

	if (rid == NULL || page == NULL || buf == NULL || n == 0 ||
	    n > EF_CHUNK)
		return 0;
	for (i = 1; i < n; i++) {
		if (rid[i] <= rid[i - 1] || page[i] < page[i - 1] ||
		    page[i] - page[i - 1] > EF_MAX_SKIP + 1u)
			return 0;
		if (page[i] != page[i - 1])
			np++;
	}
	base = ef_u(rid[0]);
	span = ef_u(rid[n - 1]) - base;
	l = ef_lbits(span, n);
	uw = EF_W((uint32_t)(span >> l) + n);
	lw = EF_W(n * l);
	len = EF_HDR + 8u * uw + 8u * lw + 2u * np;
	if (len > cap)
		return 0;
	memset(buf, 0, len);

	/* Header. */
	st64(buf, base);
	st16(buf + 8, n);
	buf[10] = (uint8_t)l;
	st16(buf + 12, uw);
	st16(buf + 14, np);
	buf[16] = (uint8_t)page[0];
	buf[17] = (uint8_t)(page[0] >> 8);
	buf[18] = (uint8_t)(page[0] >> 16);
	buf[19] = (uint8_t)(page[0] >> 24);
	up = buf + EF_HDR;
	lo = up + 8u * uw;
	pm = lo + 8u * lw;

	/* Upper bits as unary gaps, low bits packed, page starts. */
	for (i = 0, np = 0; i < n; i++) {
		v = ef_u(rid[i]) - base;
		bit = (v >> l) + i;
		up[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
		if (l > 0) {
			uint64_t lb = v & (l == 64u ? ~0ull : (1ull << l) - 1u);
			uint64_t at = (uint64_t)i * l;
			uint32_t k;
			for (k = 0; k < l; k++, at++)
				if ((lb >> k) & 1u)
					lo[at / 8u] |= (uint8_t)(1u << (at % 8u));
		}
		if (i > 0 && page[i] != page[i - 1]) {
			step = page[i] - page[i - 1] - 1u;
			st16(pm + 2u * np++, i | step << 9);
		}
	}
	return len;
}

int
ef_chunk_valid(const uint8_t *buf, size_t len)
{
	struct ef_view v;
	uint32_t i, ones = 0, prev = 0, r;

	if (buf == NULL || len < EF_HDR)
		return 0;
	ef_view(buf, &v);
	if (v.n == 0 || v.n > EF_CHUNK || v.l > 63u || buf[11] != 0 ||
	    v.np >= v.n || v.uw > EF_W(3u * EF_CHUNK) ||
	    len != EF_HDR + 8u * v.uw + 8u * EF_W(v.n * v.l) + 2u * v.np)
		return 0;
	for (i = 0; i < v.uw; i++)
		ones += (uint32_t)__builtin_popcountll(ld64(v.up + 8u * i));
	if (ones != v.n)
		return 0;
	for (i = 0; i < v.np; i++) {
		r = ld16(v.pm + 2u * i) & 0x1FFu;
		if (r == 0 || r >= v.n || r <= prev)
			return 0;
		prev = r;
	}
	return 1;
}

uint32_t
ef_chunk_count(const uint8_t *buf)
{
	return ld16(buf + 8);
}

int64_t
ef_chunk_first(const uint8_t *buf)
{
	return ef_s(ld64(buf));
}

/* Position of the set bit of element r (r < n). */
static uint32_t
ef_select1(const struct ef_view *v, uint32_t r)
{
	uint32_t w, c;
	uint64_t x;

	for (w = 0;; w++) {
		x = ld64(v->up + 8u * w);
		c = (uint32_t)__builtin_popcountll(x);
		if (r < c)
			break;
		r -= c;
	}
	while (r-- > 0)
		x &= x - 1u;             /* drop the lowest set bit */
	return w * 64u + (uint32_t)__builtin_ctzll(x);
}

int64_t
ef_chunk_get(const uint8_t *buf, uint32_t r)
{
	struct ef_view v;
	uint32_t p;

	ef_view(buf, &v);
	p = ef_select1(&v, r);
	return ef_s(v.base + (((uint64_t)(p - r) << v.l) | ef_low(&v, r)));
}

int64_t
ef_chunk_last(const uint8_t *buf)
{
	return ef_chunk_get(buf, ef_chunk_count(buf) - 1u);
}

uint32_t
ef_chunk_page(const uint8_t *buf, uint32_t r)
{
	struct ef_view v;
	uint32_t pg, i, e;

	ef_view(buf, &v);
	pg = v.page0;
	for (i = 0; i < v.np; i++) {
		e = ld16(v.pm + 2u * i);
		if ((e & 0x1FFu) > r)
			break;
		pg += 1u + (e >> 9);
	}
	return pg;
}

int
ef_chunk_find(const uint8_t *buf, int64_t rid, uint32_t *rank,
    uint32_t *page)
{
	struct ef_view v;
	uint64_t u = ef_u(rid), d, h, x, lowv;
	uint32_t w, z, p, i;

	ef_view(buf, &v);
	if (u < v.base)
		return 0;
	d = u - v.base;
	h = d >> v.l;
	lowv = v.l == 0 ? 0 : d & ((1ull << v.l) - 1u);
	if (h > 64ull * v.uw)
		return 0;
	/* Skip h zeros: element i with high_i >= h starts right after the
	 * h-th zero (at bit 0 when h is 0). */
	p = 0;
	if (h > 0) {
		for (w = 0, z = 0; w < v.uw; w++) {
			x = ~ld64(v.up + 8u * w);
			if (z + (uint32_t)__builtin_popcountll(x) >= h)
				break;
			z += (uint32_t)__builtin_popcountll(x);
		}
		if (w == v.uw)
			return 0;
		while (++z < h)
			x &= x - 1u;
		p = w * 64u + (uint32_t)__builtin_ctzll(x) + 1u;
	}
	/* The ones from p on are the elements whose high part is h. */
	for (i = p - (uint32_t)h; p < 64u * v.uw && i < v.n; p++, i++) {
		if (!((ld64(v.up + 8u * (p / 64u)) >> (p % 64u)) & 1u))
			return 0;            /* past high h: absent */
		x = ef_low(&v, i);
		if (x == lowv) {
			if (rank != NULL)
				*rank = i;
			if (page != NULL)
				*page = ef_chunk_page(buf, i);
			return 1;
		}
		if (x > lowv)
			return 0;
	}
	return 0;
}

void
ef_chunk_decode(const uint8_t *buf, int64_t *out)
{
	struct ef_view v;
	uint32_t w, i = 0;
	uint64_t x;

	ef_view(buf, &v);
	for (w = 0; w < v.uw && i < v.n; w++) {
		x = ld64(v.up + 8u * w);
		while (x != 0 && i < v.n) {
			uint32_t p = w * 64u + (uint32_t)__builtin_ctzll(x);
			out[i] = ef_s(v.base +
			    (((uint64_t)(p - i) << v.l) | ef_low(&v, i)));
			i++;
			x &= x - 1u;
		}
	}
}
