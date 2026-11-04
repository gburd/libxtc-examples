/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/refbook.h -- a reference matcher that shares no code with
 * book.c.  Deliberately naive: each side is an array of resting orders
 * kept in priority order (price, then arrival), matched by a linear
 * scan.  Covers the order types the differential test drives: limit,
 * market, IOC, FOK, post-only, cancel, reduce, replace.  Correct by
 * inspection, slow by design; the fast engine must agree with it.
 */
#ifndef L2BOOK_REFBOOK_H
#define L2BOOK_REFBOOK_H

#include <stdint.h>
#include <string.h>

#define REF_MAX 4096

typedef struct {
	uint64_t id;
	int64_t price, leaves;
	uint64_t arrival;
} ref_ord_t;

typedef struct {
	ref_ord_t o[2][REF_MAX];	/* [side], sorted best first */
	int n[2];
	uint64_t clock;
	uint64_t trades;
	int64_t volume;
	int64_t last;
} ref_book_t;

/* Does a (side s) at price p come before b? */
static int
ref_before(int s, const ref_ord_t *a, const ref_ord_t *b)
{
	if (a->price != b->price)
		return s == 0 ? a->price > b->price : a->price < b->price;
	return a->arrival < b->arrival;
}

static void
ref_insert(ref_book_t *r, int s, uint64_t id, int64_t price, int64_t qty)
{
	ref_ord_t x;
	int i;
	x.id = id;
	x.price = price;
	x.leaves = qty;
	x.arrival = ++r->clock;
	for (i = r->n[s]; i > 0 && ref_before(s, &x, &r->o[s][i - 1]); i--)
		r->o[s][i] = r->o[s][i - 1];
	r->o[s][i] = x;
	r->n[s]++;
}

static int
ref_find(const ref_book_t *r, uint64_t id, int *side)
{
	int s, i;
	for (s = 0; s < 2; s++)
		for (i = 0; i < r->n[s]; i++)
			if (r->o[s][i].id == id) {
				*side = s;
				return i;
			}
	return -1;
}

static void
ref_remove(ref_book_t *r, int s, int i)
{
	memmove(&r->o[s][i], &r->o[s][i + 1],
	    (size_t)(r->n[s] - i - 1) * sizeof r->o[s][0]);
	r->n[s]--;
}

static int
ref_crosses(int s, int64_t px, int market, int64_t rp)
{
	return market || (s == 0 ? rp <= px : rp >= px);
}

static int64_t
ref_avail(const ref_book_t *r, int s, int64_t px, int market)
{
	int os = !s, i;
	int64_t q = 0;
	for (i = 0; i < r->n[os] && ref_crosses(s, px, market, r->o[os][i].price);
	    i++)
		q += r->o[os][i].leaves;
	return q;
}

/* tif: 0 GTC, 1 IOC, 2 FOK.  Returns the quantity that traded. */
static int64_t
ref_submit(ref_book_t *r, uint64_t id, int s, int market, int64_t px,
    int64_t qty, int tif, int post_only)
{
	int os = !s;
	int64_t leaves = qty, traded = 0;
	if (post_only && r->n[os] > 0 && ref_crosses(s, px, 0, r->o[os][0].price))
		return 0;
	if (tif == 2 && ref_avail(r, s, px, market) < qty)
		return 0;
	while (leaves > 0 && r->n[os] > 0 &&
	    ref_crosses(s, px, market, r->o[os][0].price)) {
		ref_ord_t *m = &r->o[os][0];
		int64_t q = leaves < m->leaves ? leaves : m->leaves;
		leaves -= q;
		m->leaves -= q;
		traded += q;
		r->trades++;
		r->volume += q;
		r->last = m->price;
		if (m->leaves == 0)
			ref_remove(r, os, 0);
	}
	if (leaves > 0 && !market && tif == 0)
		ref_insert(r, s, id, px, leaves);
	return traded;
}

static int
ref_cancel(ref_book_t *r, uint64_t id)
{
	int s, i = ref_find(r, id, &s);
	if (i < 0)
		return -1;
	ref_remove(r, s, i);
	return 0;
}

static int
ref_reduce(ref_book_t *r, uint64_t id, int64_t q)
{
	int s, i = ref_find(r, id, &s);
	if (i < 0 || q >= r->o[s][i].leaves)
		return -1;
	if (q <= 0)
		ref_remove(r, s, i);
	else
		r->o[s][i].leaves = q;
	return 0;
}

/* Total resting quantity at price p on side s. */
static int64_t
ref_level(const ref_book_t *r, int s, int64_t p)
{
	int64_t q = 0;
	int i;
	for (i = 0; i < r->n[s]; i++)
		if (r->o[s][i].price == p)
			q += r->o[s][i].leaves;
	return q;
}

#endif /* L2BOOK_REFBOOK_H */
