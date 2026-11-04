/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/hist.h -- a log-linear latency histogram (HdrHistogram's
 * shape): exact below 64 ns, then 32 sub-buckets per power of two, so a
 * reported percentile is the bucket's lower bound, within 3.1 % of the
 * true sample.  Constant memory, O(1) record, no allocation.
 */
#ifndef L2BOOK_HIST_H
#define L2BOOK_HIST_H

#include <stdint.h>
#include <string.h>

#define HIST_SUB	32
#define HIST_EXACT	64
#define HIST_POWERS	40	/* up to 2^(6+40) ns */
#define HIST_N		(HIST_EXACT + HIST_POWERS * HIST_SUB)

typedef struct {
	uint64_t c[HIST_N];
	uint64_t n, max;
} hist_t;

static inline unsigned
hist_bucket(uint64_t v)
{
	unsigned p;
	if (v < HIST_EXACT)
		return (unsigned)v;
	p = 63u - (unsigned)__builtin_clzll(v);	/* >= 6 */
	if (p - 6 >= HIST_POWERS)
		return HIST_N - 1;
	return HIST_EXACT + (p - 6) * HIST_SUB +
	    (unsigned)((v >> (p - 5)) & (HIST_SUB - 1));
}

static inline uint64_t
hist_lower(unsigned b)
{
	unsigned p, s;
	if (b < HIST_EXACT)
		return b;
	p = (b - HIST_EXACT) / HIST_SUB + 6;
	s = (b - HIST_EXACT) % HIST_SUB;
	return (1ull << p) + ((uint64_t)s << (p - 5));
}

static inline void
hist_record(hist_t *h, uint64_t v)
{
	h->c[hist_bucket(v)]++;
	h->n++;
	if (v > h->max)
		h->max = v;
}

static inline void
hist_merge(hist_t *a, const hist_t *b)
{
	unsigned i;
	for (i = 0; i < HIST_N; i++)
		a->c[i] += b->c[i];
	a->n += b->n;
	if (b->max > a->max)
		a->max = b->max;
}

/* The value at quantile q (0..1), as a bucket lower bound. */
static inline uint64_t
hist_q(const hist_t *h, double q)
{
	uint64_t want, acc = 0;
	unsigned i;
	if (h->n == 0)
		return 0;
	want = (uint64_t)(q * (double)h->n);
	if (want >= h->n)
		want = h->n - 1;
	for (i = 0; i < HIST_N; i++) {
		acc += h->c[i];
		if (acc > want)
			return hist_lower(i);
	}
	return h->max;
}

#endif /* L2BOOK_HIST_H */
