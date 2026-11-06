/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/book.c -- the matching engine.  See book.h for the shape.
 *
 *	Price-time priority.  A price inside the window is an index into a
 *	dense ladder of BOOK_LADDER levels starting at b->base.  Each side
 *	keeps a bitmap of the window's non-empty levels under a one-word
 *	summary, so "best bid" is the highest set bit and "best ask" the
 *	lowest, two find-first-set instructions, and walking the book is a
 *	scan of set bits.
 *
 *	Any valid price rests.  A level outside the window lives in its
 *	side's overflow list (sorted by price); the walks fall through to it
 *	when the window runs out.  At the end of every command settle()
 *	moves the window: onto the whole book when it fits, else onto the
 *	touch.  For US equities a 4096-cent window is +-$20 on a penny tick,
 *	wider than any LULD band, so the overflow and re-bases are the edge
 *	case, not the hot path: with the overflow empty, each best-price
 *	lookup, level step and level lookup costs one predicted branch.
 */
#include "book.h"

#if defined(__wasm__) && !defined(__wasi__)
/* Freestanding wasm32 (the browser console): no libc.  The engine needs
 * three mem functions; the compiler lowers these builtins to wasm bulk
 * memory or a loop. */
#define memset(d, c, n)		__builtin_memset((d), (c), (n))
#define memcpy(d, s, n)		__builtin_memcpy((d), (s), (n))
#define memmove(d, s, n)	__builtin_memmove((d), (s), (n))
#else
#include <string.h>
#endif

/* ---- bit tricks ---------------------------------------------------- */

static inline int
ctz64(uint64_t x)
{
	return __builtin_ctzll(x);
}

static inline int
clz64(uint64_t x)
{
	return __builtin_clzll(x);
}

static inline int
hi_bit(uint64_t x)	/* index of the highest set bit; x != 0 */
{
	return 63 - clz64(x);
}

/* ---- ladder -------------------------------------------------------- */

/*
 * A level handle names a level of one side: h < BOOK_LADDER is window
 * index h, the price base + h * tick; h >= BOOK_LADDER is overflow slot
 * h - BOOK_LADDER, whose price is stored in it.  Handles are valid
 * within one command: the window moves only in settle(), after the
 * command's last walk.  A freed overflow slot keeps its price, and the
 * walks navigate by price, so a handle to a level that emptied under a
 * walk (an OCO partner cancelled ahead of it) still finds the next one.
 */
#define XH(x) ((uint32_t)(x) + BOOK_LADDER)
#define IN_WIN(h) __builtin_expect((h) < BOOK_LADDER, 1)

static inline int64_t
win_top(const book_t *b) /* first price above the window */
{
	return b->base + (int64_t)BOOK_LADDER * b->cfg.tick;
}

/* One compare: prices and the base are positive and below
 * BOOK_PRICE_MAX, so price - base cannot overflow. */
static inline int
in_win(const book_t *b, int64_t price)
{
	return (uint64_t)(price - b->base) <
	    (uint64_t)BOOK_LADDER * (uint64_t)b->cfg.tick;
}

static inline uint32_t
lv_index(const book_t *b, int64_t price) /* in_win(price) */
{
	return (uint32_t)((price - b->base) / b->cfg.tick);
}

static inline int64_t
lv_price(const book_t *b, uint32_t h)
{
	if (IN_WIN(h))
		return b->base + (int64_t)h * b->cfg.tick;
	return b->xlv[h - BOOK_LADDER].price;
}

static inline book_level_t *
lv_at(book_t *b, int s, uint32_t h)
{
	return IN_WIN(h) ? &b->side[s].lv[h] : &b->xlv[h - BOOK_LADDER].lv;
}

static inline const book_level_t *
lv_cat(const book_t *b, int s, uint32_t h)
{
	return IN_WIN(h) ? &b->side[s].lv[h] : &b->xlv[h - BOOK_LADDER].lv;
}

/* A price the book accepts: a positive whole number of ticks. */
static inline int
valid_price(const book_t *b, int64_t price)
{
	return price > 0 && price <= BOOK_PRICE_MAX && price % b->cfg.tick == 0;
}

/* Is price a strictly worse than c for side s (a bid lower, an ask
 * higher)? */
static inline int
worse(int s, int64_t a, int64_t c)
{
	return s == BOOK_BUY ? a < c : a > c;
}

static inline void
ladder_set(book_ladder_t *l, uint32_t i)
{
	l->bits[i >> 6] |= 1ull << (i & 63);
	l->summary |= 1ull << (i >> 6);
}

static inline void
ladder_clear(book_ladder_t *l, uint32_t i)
{
	l->bits[i >> 6] &= ~(1ull << (i & 63));
	if (l->bits[i >> 6] == 0)
		l->summary &= ~(1ull << (i >> 6));
}

/* Highest non-empty level index, or BOOK_NONE. */
static inline uint32_t
ladder_max(const book_ladder_t *l)
{
	int w;
	if (l->summary == 0)
		return BOOK_NONE;
	w = hi_bit(l->summary);
	return (uint32_t)(w * 64 + hi_bit(l->bits[w]));
}

/* Lowest non-empty level index, or BOOK_NONE. */
static inline uint32_t
ladder_min(const book_ladder_t *l)
{
	int w;
	if (l->summary == 0)
		return BOOK_NONE;
	w = ctz64(l->summary);
	return (uint32_t)(w * 64 + ctz64(l->bits[w]));
}

/* Next non-empty level strictly below i, or BOOK_NONE. */
static uint32_t
ladder_below(const book_ladder_t *l, uint32_t i)
{
	uint32_t w = i >> 6;
	uint64_t m;
	if (i == 0)
		return BOOK_NONE;
	m = (i & 63) ? l->bits[w] & ((1ull << (i & 63)) - 1) : 0;
	if (m)
		return w * 64 + (uint32_t)hi_bit(m);
	m = w ? l->summary & ((1ull << w) - 1) : 0;
	if (!m)
		return BOOK_NONE;
	w = (uint32_t)hi_bit(m);
	return w * 64 + (uint32_t)hi_bit(l->bits[w]);
}

/* Next non-empty level strictly above i, or BOOK_NONE. */
static uint32_t
ladder_above(const book_ladder_t *l, uint32_t i)
{
	uint32_t w = i >> 6;
	uint64_t m;
	if (i + 1 >= BOOK_LADDER)
		return BOOK_NONE;
	m = ((i & 63) == 63) ? 0 : l->bits[w] & ~((2ull << (i & 63)) - 1);
	if (m)
		return w * 64 + (uint32_t)ctz64(m);
	m = (w == BOOK_WORDS - 1) ? 0 : l->summary & ~((2ull << w) - 1);
	if (!m)
		return BOOK_NONE;
	w = (uint32_t)ctz64(m);
	return w * 64 + (uint32_t)ctz64(l->bits[w]);
}

/* ---- overflow ------------------------------------------------------ */

/*
 * The overflow store holds the levels outside the window.  Per side,
 * xord[s][0 .. nx[s]) are slot numbers sorted worst price first, best
 * last, so a side's overflow is two runs: the levels worse than the
 * window (a prefix) and the levels better than it (a suffix).  A slot
 * is in at most one list, and a slot in no list is on the free list.
 */

/* How many of side s's overflow levels are strictly worse than p. */
static uint32_t
xrank(const book_t *b, int s, int64_t p)
{
	uint32_t lo = 0, hi = b->nx[s];
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (worse(s, b->xlv[b->xord[s][mid]].price, p))
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* Side s's overflow levels worse than the whole window. */
static inline uint32_t
xworse(const book_t *b, int s)
{
	return s == BOOK_BUY ? xrank(b, s, b->base) :
	                       xrank(b, s, win_top(b) - b->cfg.tick);
}

/* Is overflow level price p better than the whole window? */
static inline int
x_better(const book_t *b, int s, int64_t p)
{
	return s == BOOK_BUY ? p >= win_top(b) : p < b->base;
}

/* Best window index of a side (bid: highest, ask: lowest). */
static inline uint32_t
win_best(const book_t *b, int s)
{
	return s == BOOK_BUY ? ladder_max(&b->side[0]) :
	    ladder_min(&b->side[1]);
}

/* Best level of a side: the window's best unless the side's best
 * overflow level is better than the window, or the window is empty. */
static inline uint32_t
side_best(const book_t *b, int s)
{
	uint32_t i = win_best(b, s), x;
	if (__builtin_expect(b->nx[s] == 0, 1))
		return i;
	x = b->xord[s][b->nx[s] - 1];
	if (i == BOOK_NONE || x_better(b, s, b->xlv[x].price))
		return XH(x);
	return i;
}

/*
 * The next level of side s away from the touch, once the window has
 * none past h or h is an overflow level.  The overflow's worse run lies
 * past the window's worst level, its better run before the window's
 * best, so from the better run the window comes next.
 */
static uint32_t
side_next_x(const book_t *b, int s, uint32_t h)
{
	uint32_t k, c = BOOK_NONE, i;
	int64_t p;

	if (h < BOOK_LADDER) {
		k = xworse(b, s);
		return k > 0 ? XH(b->xord[s][k - 1]) : BOOK_NONE;
	}
	p = lv_price(b, h);
	k = xrank(b, s, p);
	if (k > 0)
		c = b->xord[s][k - 1];
	if (x_better(b, s, p) &&
	    (c == BOOK_NONE || !x_better(b, s, b->xlv[c].price)) &&
	    (i = win_best(b, s)) != BOOK_NONE)
		return i;
	return c == BOOK_NONE ? BOOK_NONE : XH(c);
}

/* The next level away from the touch. */
static inline uint32_t
side_next(const book_t *b, int s, uint32_t h)
{
	uint32_t i = BOOK_NONE;
	if (h < BOOK_LADDER)
		i = s == BOOK_BUY ? ladder_below(&b->side[0], h) :
		                    ladder_above(&b->side[1], h);
	if (__builtin_expect(i != BOOK_NONE || b->nx[s] == 0, 1))
		return i;
	return side_next_x(b, s, h);
}

/* ---- events -------------------------------------------------------- */

static book_event_t *
emit(book_t *b, book_out_t *out, uint8_t kind)
{
	book_event_t *e;
	static book_event_t sink;

	b->seq++;
	if (out == NULL || out->n >= out->cap) {
		if (out != NULL)
			out->overflow++;
		memset(&sink, 0, sizeof sink);
		return &sink;
	}
	e = &out->ev[out->n++];
	memset(e, 0, sizeof *e);
	e->kind = kind;
	e->seq = b->seq;
	return e;
}

static void
emit_level(book_t *b, book_out_t *out, int s, uint32_t h)
{
	book_event_t *e = emit(b, out, BOOK_EV_LEVEL);
	e->side = (uint8_t)s;
	e->price = lv_price(b, h);
	e->qty = lv_cat(b, s, h)->qty;
}

static void
emit_order(book_t *b, book_out_t *out, uint8_t kind, const book_slot_t *o,
    uint8_t reason)
{
	book_event_t *e = emit(b, out, kind);
	e->side = o->side;
	e->id = o->id;
	e->price = o->price;
	e->qty = o->leaves;
	e->account = o->account;
	e->reason = reason;
}

static void
emit_reject(book_t *b, book_out_t *out, const book_order_t *o, uint8_t reason)
{
	book_event_t *e = emit(b, out, BOOK_EV_REJECTED);
	e->side = o->side;
	e->id = o->id;
	e->price = o->price;
	e->qty = o->qty;
	e->account = o->account;
	e->reason = reason;
}

/* ---- id map -------------------------------------------------------- */

#define MAP_EMPTY	0ull

static inline uint32_t
map_hash(uint64_t id)
{
	id ^= id >> 33;
	id *= 0xff51afd7ed558ccdull;
	id ^= id >> 33;
	return (uint32_t)id;
}

static uint32_t
map_get(const book_t *b, uint64_t id)
{
	uint32_t h = map_hash(id) & b->map_mask;
	for (;;) {
		uint64_t k = b->map_id[h];
		if (k == id)
			return b->map_ix[h];
		if (k == MAP_EMPTY)
			return BOOK_NONE;
		h = (h + 1) & b->map_mask;
	}
}

static int
map_put(book_t *b, uint64_t id, uint32_t ix)
{
	uint32_t h;
	if (b->map_used + 1 > (b->map_mask + 1) / 4 * 3)
		return -1;
	h = map_hash(id) & b->map_mask;
	for (;;) {
		uint64_t k = b->map_id[h];
		if (k == id)
			return -1;
		if (k == MAP_EMPTY)
			break;
		h = (h + 1) & b->map_mask;
	}
	b->map_used++;
	b->map_id[h] = id;
	b->map_ix[h] = ix;
	return 0;
}

/*
 * Backward-shift deletion (Knuth, TAOCP vol. 3, 6.4 Algorithm R): no
 * tombstones, so map_used counts live ids only and a long-lived book
 * never fills its map with the ids of orders long gone.  Each later
 * entry of the run moves into the hole if the hole lies on its probe
 * path (between its home slot and where it sits).
 */
static void
map_del(book_t *b, uint64_t id)
{
	uint32_t m = b->map_mask, h = map_hash(id) & m, j;
	for (;;) {
		uint64_t k = b->map_id[h];
		if (k == id)
			break;
		if (k == MAP_EMPTY)
			return;
		h = (h + 1) & m;
	}
	for (j = (h + 1) & m; b->map_id[j] != MAP_EMPTY; j = (j + 1) & m) {
		uint32_t home = map_hash(b->map_id[j]) & m;
		if (((j - home) & m) >= ((j - h) & m)) {
			b->map_id[h] = b->map_id[j];
			b->map_ix[h] = b->map_ix[j];
			h = j;
		}
	}
	b->map_id[h] = MAP_EMPTY;
	b->map_used--;
}

/* ---- pool ---------------------------------------------------------- */

static uint32_t
pool_get(book_t *b)
{
	uint32_t ix = b->free_head;
	if (ix == BOOK_NONE)
		return BOOK_NONE;
	b->free_head = b->pool[ix].next;
	b->n_live++;
	return ix;
}

static void
pool_put(book_t *b, uint32_t ix)
{
	memset(&b->pool[ix], 0, sizeof b->pool[ix]);
	b->pool[ix].next = b->free_head;
	b->free_head = ix;
	b->n_live--;
}

/* ---- overflow slots ------------------------------------------------ */

static inline void
lv_clear(book_level_t *lv)
{
	lv->head = lv->tail = BOOK_NONE;
	lv->qty = 0;
	lv->count = 0;
	lv->xnext = 0;
}

/* A free slot for a level at price; there is always one (see book.h). */
static uint32_t
x_alloc(book_t *b, int64_t price)
{
	uint32_t x = b->xfree;
	b->xfree = b->xlv[x].lv.xnext;
	lv_clear(&b->xlv[x].lv);
	b->xlv[x].price = price;
	return x;
}

/* The slot keeps its price and an empty level, so a walk that holds its
 * handle still reads price, qty 0, and finds the next level. */
static inline void
x_free(book_t *b, uint32_t x)
{
	lv_clear(&b->xlv[x].lv);
	b->xlv[x].lv.xnext = b->xfree;
	b->xfree = x;
}

/* The handle of side s's level at price, or BOOK_NONE if it has none. */
static uint32_t
lv_find(const book_t *b, int s, int64_t price)
{
	uint32_t k;
	if (in_win(b, price))
		return lv_index(b, price);
	k = xrank(b, s, price);
	if (k < b->nx[s] && b->xlv[b->xord[s][k]].price == price)
		return XH(b->xord[s][k]);
	return BOOK_NONE;
}

/* The handle of side s's level at price, made (empty) if need be. */
static uint32_t
lv_get(book_t *b, int s, int64_t price)
{
	uint32_t k, x, *a = b->xord[s];
	if (in_win(b, price))
		return lv_index(b, price);
	k = xrank(b, s, price);
	if (k < b->nx[s] && b->xlv[a[k]].price == price)
		return XH(a[k]);
	x = x_alloc(b, price);
	memmove(&a[k + 1], &a[k], (b->nx[s] - k) * sizeof *a);
	a[k] = x;
	b->nx[s]++;
	return XH(x);
}

/* Side s's overflow level x has emptied: out of the list, slot freed. */
static void
x_remove(book_t *b, int s, uint32_t x)
{
	uint32_t k = xrank(b, s, b->xlv[x].price), *a = b->xord[s];
	memmove(&a[k], &a[k + 1], (b->nx[s] - k - 1) * sizeof *a);
	b->nx[s]--;
	x_free(b, x);
}

/* ---- level queue --------------------------------------------------- */

static void
level_push(book_t *b, int s, uint32_t li, uint32_t ix)
{
	book_level_t *lv = lv_at(b, s, li);
	book_slot_t *o = &b->pool[ix];

	o->next = BOOK_NONE;
	o->prev = lv->tail;
	if (lv->tail != BOOK_NONE)
		b->pool[lv->tail].next = ix;
	else
		lv->head = ix;
	lv->tail = ix;
	lv->qty += o->shown;
	if (lv->count++ == 0) {
		b->moved = 1;
		if (li < BOOK_LADDER) {
			ladder_set(&b->side[s], li);
			b->side[s].nlevels++;
		}
	}
}

static void
level_unlink(book_t *b, int s, uint32_t li, uint32_t ix)
{
	book_level_t *lv = lv_at(b, s, li);
	book_slot_t *o = &b->pool[ix];

	if (o->prev != BOOK_NONE)
		b->pool[o->prev].next = o->next;
	else
		lv->head = o->next;
	if (o->next != BOOK_NONE)
		b->pool[o->next].prev = o->prev;
	else
		lv->tail = o->prev;
	lv->qty -= o->shown;
	if (--lv->count == 0) {
		lv->head = lv->tail = BOOK_NONE;
		b->moved = 1;
		if (li >= BOOK_LADDER)
			x_remove(b, s, li - BOOK_LADDER);
		else {
			ladder_clear(&b->side[s], li);
			b->side[s].nlevels--;
		}
	}
}

/* An iceberg's next peak. */
static inline int64_t
peak(const book_slot_t *o)
{
	return o->leaves < o->display ? o->leaves : o->display;
}

/* An iceberg's peak refills to `shown` and goes to the back of its level.
 * The level never empties on the way, so a walk's handles stay put. */
static void
level_requeue(book_t *b, int s, uint32_t li, uint32_t ix, int64_t shown)
{
	book_level_t *lv = lv_at(b, s, li);
	book_slot_t *o = &b->pool[ix];

	lv->qty += shown - o->shown;
	o->shown = shown;
	if (lv->tail == ix)
		return;
	if (o->prev != BOOK_NONE)
		b->pool[o->prev].next = o->next;
	else
		lv->head = o->next;
	b->pool[o->next].prev = o->prev;
	o->prev = lv->tail;
	o->next = BOOK_NONE;
	b->pool[lv->tail].next = ix;
	lv->tail = ix;
}

/* ---- the window moves ---------------------------------------------- */

/* Price of side s's worst level, or BOOK_NO_PRICE if the side is empty. */
static int64_t
side_worst(const book_t *b, int s)
{
	uint32_t w =
	    s == BOOK_BUY ? ladder_min(&b->side[0]) : ladder_max(&b->side[1]);
	if (b->nx[s] > 0) {
		int64_t x0 = b->xlv[b->xord[s][0]].price;
		if (w == BOOK_NONE || !x_better(b, s, x0))
			return x0;
	}
	return w == BOOK_NONE ? BOOK_NO_PRICE : lv_price(b, w);
}

static inline int64_t
dist(int64_t a, int64_t c)
{
	return a > c ? a - c : c - a;
}

/* The base that centres the window on [lo, hi] (which fits in it). */
static int64_t
centre(const book_t *b, int64_t lo, int64_t hi)
{
	int64_t t = b->cfg.tick, nb;
	int64_t slack = (int64_t)BOOK_LADDER - 1 - (hi - lo) / t;
	nb = lo - slack / 2 * t;
	return nb < t ? t : nb; /* no window slot below the tick */
}

/*
 * Where the window belongs, the invariant settle() keeps after every
 * command (book_check verifies it):
 *   - the overflow is empty (every level is in the window), or
 *   - the levels of both sides span more than the window, and it holds
 *     the touch: both best prices if they fit in it together, else at
 *     least one of them.
 * Returns 1 and the new base if the window must move to restore it.
 */
static int
settle_base(const book_t *b, int64_t *nb)
{
	int64_t t = b->cfg.tick, lo = INT64_MAX, hi = INT64_MIN, p[2], m;
	int s;

	if (b->nx[0] + b->nx[1] == 0)
		return 0;
	for (s = 0; s < 2; s++) {
		int64_t w = side_worst(b, s);
		p[s] = book_best(b, (book_side_t)s);
		if (p[s] == BOOK_NO_PRICE)
			continue;
		lo = p[s] < lo ? p[s] : lo;
		hi = p[s] > hi ? p[s] : hi;
		lo = w < lo ? w : lo;
		hi = w > hi ? w : hi;
	}
	if ((hi - lo) / t < (int64_t)BOOK_LADDER) {
		*nb = centre(b, lo, hi); /* everything fits: take it all */
		return *nb != b->base;
	}
	if (p[0] != BOOK_NO_PRICE && p[1] != BOOK_NO_PRICE) {
		lo = p[0] < p[1] ? p[0] : p[1];
		hi = p[0] < p[1] ? p[1] : p[0];
		if ((hi - lo) / t < (int64_t)BOOK_LADDER) {
			if (in_win(b, lo) && in_win(b, hi))
				return 0;
			*nb = centre(b, lo, hi);
			return 1;
		}
		if (in_win(b, p[0]) || in_win(b, p[1]))
			return 0;
		/* Neither touch: the one nearer the window's middle. */
		m = b->base + (int64_t)(BOOK_LADDER / 2) * t;
		s = dist(p[0], m) <= dist(p[1], m) ? BOOK_BUY : BOOK_SELL;
	} else
		s = p[0] != BOOK_NO_PRICE ? 0 : 1;
	if (in_win(b, p[s]))
		return 0;
	*nb = centre(b, p[s], p[s]);
	return 1;
}

/* Set bits of w in [lo, hi). */
static uint32_t
bits_count(const uint64_t *w, uint32_t lo, uint32_t hi)
{
	uint32_t n = 0, i = lo;
	for (; i < hi && (i & 63) != 0; i++)
		n += (uint32_t)(w[i >> 6] >> (i & 63)) & 1;
	for (; i + 64 <= hi; i += 64)
		n += (uint32_t)__builtin_popcountll(w[i >> 6]);
	for (; i < hi; i++)
		n += (uint32_t)(w[i >> 6] >> (i & 63)) & 1;
	return n;
}

/* The bitmap of a window moved up by d levels (d < 0: down): new bit j
 * is old bit j + d; bits shifted out of range are dropped. */
static void
bits_shift(uint64_t *w, int64_t d)
{
	uint64_t nw[BOOK_WORDS];
	int64_t q = d >= 0 ? d / 64 : -((63 - d) / 64), a;
	unsigned r = (unsigned)(d - q * 64);
	uint32_t i;

	for (i = 0; i < BOOK_WORDS; i++) {
		uint64_t lo, hi;
#ifdef PLANT_REBASE_NOBITMAP
		/* Planted bug: one word of the bitmap is not shifted. */
		if (i == BOOK_WORDS / 2) {
			nw[i] = w[i];
			continue;
		}
#endif
		a = (int64_t)i + q;
		lo = a >= 0 && a < (int64_t)BOOK_WORDS ? w[a] : 0;
		hi = a + 1 >= 0 && a + 1 < (int64_t)BOOK_WORDS ? w[a + 1] : 0;
		nw[i] = r ? lo >> r | hi << (64 - r) : lo;
	}
	memcpy(w, nw, sizeof nw);
}

/*
 * Move side s's half of the window to base nb, levels as whole structs
 * (their FIFOs are pool indices, and nothing else holds a window index:
 * orders and stops keep prices), so no order is touched.  The levels
 * that leave go to the overflow as one block: the window held no
 * overflow level, so they all sort into the gap between its two runs.
 * The rest shift; the bitmap shifts by words.  Cost: the levels that
 * leave, the occupied span that moves, and BOOK_WORDS words.
 */
static void
rebase_side(book_t *b, int s, int64_t d)
{
	const int64_t L = (int64_t)BOOK_LADDER;
	book_ladder_t *l = &b->side[s];
	uint32_t *a = b->xord[s], wl = ladder_min(l), wh, rlo, rhi, m, k, i;
	int64_t sl, sh, dl, dh;

	if (wl == BOOK_NONE)
		return;
	wh = ladder_max(l);
	/* Old indices [rlo, rhi) leave: the end the window moves away from. */
	rlo = 0;
	rhi = BOOK_LADDER;
	if (d > 0 && d < L)
		rhi = (uint32_t)d;
	else if (d < 0 && d > -L)
		rlo = (uint32_t)(L + d);
	m = bits_count(l->bits, rlo, rhi);
	if (m > 0) {
		k = xworse(b, s);
		memmove(&a[k + m], &a[k], (b->nx[s] - k) * sizeof *a);
		b->nx[s] += m;
		/* worst first: bids upward, asks downward */
		if (s == BOOK_BUY)
			i = rlo == 0 ? ladder_min(l) : ladder_above(l, rlo - 1);
		else
			i = rhi == BOOK_LADDER ? ladder_max(l) :
			                         ladder_below(l, rhi);
		for (; i != BOOK_NONE && i >= rlo && i < rhi;
		     i = s == BOOK_BUY ? ladder_above(l, i) :
		                         ladder_below(l, i)) {
			uint32_t x = x_alloc(b, lv_price(b, i));
			b->xlv[x].lv = l->lv[i];
			b->xlv[x].lv.xnext = 0;
			a[k++] = x;
		}
	}
	/* The occupied survivors [sl, sh) move to [dl, dh). */
	sl = (int64_t)wl > d ? (int64_t)wl : d;
	sh = (int64_t)wh + 1 < L + d ? (int64_t)wh + 1 : L + d;
	if (sl < sh) {
		memmove(&l->lv[sl - d], &l->lv[sl],
		    (size_t)(sh - sl) * sizeof l->lv[0]);
		dl = sl - d;
		dh = sh - d;
	} else
		dl = dh = (int64_t)wh + 1;
	/* What the move did not overwrite is stale: [wl, dl) and [dh, wh]. */
	for (i = wl; (int64_t)i < dl && i <= wh; i++)
		lv_clear(&l->lv[i]);
	for (i = dh > (int64_t)wl ? (uint32_t)dh : wl; i <= wh; i++)
		lv_clear(&l->lv[i]);
	bits_shift(l->bits, d);
}

/* Move the window to base nb, then pull in the overflow levels that are
 * inside it and rebuild each side's summary and level count. */
static void
rebase(book_t *b, int64_t nb)
{
	int64_t t = b->cfg.tick, d = (nb - b->base) / t;
	uint32_t k, k1, k2, w, *a;
	int s;

	for (s = 0; s < 2; s++)
		rebase_side(b, s, d);
	b->base = nb;
	for (s = 0; s < 2; s++) {
		book_ladder_t *l = &b->side[s];
		a = b->xord[s];
		k1 = xworse(b, s);
		k2 = xrank(b, s, s == BOOK_BUY ? win_top(b) : b->base - t);
		for (k = k1; k < k2; k++) {
			uint32_t x = a[k], i = lv_index(b, b->xlv[x].price);
			l->lv[i] = b->xlv[x].lv;
			l->lv[i].xnext = 0;
			l->bits[i >> 6] |= 1ull << (i & 63);
			x_free(b, x);
		}
		memmove(&a[k1], &a[k2], (b->nx[s] - k2) * sizeof *a);
		b->nx[s] -= k2 - k1;
		l->summary = 0;
		l->nlevels = 0;
		for (w = 0; w < BOOK_WORDS; w++) {
			if (l->bits[w])
				l->summary |= 1ull << w;
			l->nlevels +=
			    (uint32_t)__builtin_popcountll(l->bits[w]);
		}
	}
	b->rebases++;
}

/*
 * End of every command: put the window where it belongs.  With the
 * overflow empty (the common case) this is one test.  Where the window
 * belongs depends only on the base and on which levels exist, so it is
 * worked out again only after a level appeared or emptied.
 */
static inline void
settle(book_t *b)
{
	int64_t nb;
	if (__builtin_expect((b->nx[0] | b->nx[1]) == 0 || !b->moved, 1))
		return;
	b->moved = 0;
	if (settle_base(b, &nb))
		rebase(b, nb);
}

/* ---- stops --------------------------------------------------------- */

/* Buy stops trigger when the price rises to the stop (ascending order:
 * the lowest triggers first); sell stops when it falls (descending). */
static int
stop_before(const book_t *b, int s, uint32_t a, uint32_t c)
{
	const book_slot_t *x = &b->pool[a], *y = &b->pool[c];
	if (x->stop != y->stop)
		return s == BOOK_BUY ? x->stop < y->stop : x->stop > y->stop;
	return 0;	/* equal triggers keep arrival order */
}

static void
stop_insert(book_t *b, int s, uint32_t ix)
{
	uint32_t n = b->n_stops[s], i = n;
	uint32_t *a = b->stops[s];
	while (i > 0 && stop_before(b, s, ix, a[i - 1])) {
		a[i] = a[i - 1];
		i--;
	}
	a[i] = ix;
	b->n_stops[s] = n + 1;
}

static void
stop_remove(book_t *b, int s, uint32_t ix)
{
	uint32_t i, n = b->n_stops[s];
	uint32_t *a = b->stops[s];
	for (i = 0; i < n; i++)
		if (a[i] == ix) {
			memmove(&a[i], &a[i + 1], (n - i - 1) * sizeof *a);
			b->n_stops[s] = n - 1;
			return;
		}
}

/* ---- removal ------------------------------------------------------- */

static void cancel_ix(book_t *b, uint32_t ix, uint8_t reason,
    book_out_t *out);

/* Take a resting order out of the book (not a stop). */
static void
rest_remove(book_t *b, uint32_t ix, book_out_t *out)
{
	book_slot_t *o = &b->pool[ix];
	uint32_t li = lv_find(b, o->side, o->price);
	level_unlink(b, o->side, li, ix);
	emit_level(b, out, o->side, li);
}

static void
release(book_t *b, uint32_t ix)
{
	map_del(b, b->pool[ix].id);
	pool_put(b, ix);
}

/* An OCO partner of a filled or cancelled order is cancelled. */
static void
oco_kill(book_t *b, uint64_t partner, book_out_t *out)
{
	uint32_t px;
	if (partner == 0)
		return;
	px = map_get(b, partner);
	if (px != BOOK_NONE && b->pool[px].live)
		cancel_ix(b, px, BOOK_R_OCO, out);
}

static void
cancel_ix(book_t *b, uint32_t ix, uint8_t reason, book_out_t *out)
{
	book_slot_t *o = &b->pool[ix];
	uint64_t partner = o->oco;

	if (o->live == 1)
		rest_remove(b, ix, out);
	else if (o->live == 2)
		stop_remove(b, o->side, ix);
	o->live = 0;
	emit_order(b, out, BOOK_EV_CANCELED, o, reason);
	release(b, ix);
	oco_kill(b, partner, out);
}

/* ---- band / circuit breaker ---------------------------------------- */

static int
outside_band(const book_t *b, int64_t price)
{
	int64_t lim;
	if (b->cfg.band_pct_bp <= 0 || b->ref <= 0)
		return 0;
	lim = b->ref * b->cfg.band_pct_bp / 10000;
	return price > b->ref + lim || price < b->ref - lim;
}

/* ---- matching ------------------------------------------------------ */

/* Can an incoming order of side s at limit px trade against level i? */
static inline int
crosses(const book_t *b, int s, int64_t px, uint32_t i, int is_market)
{
	int64_t lp = lv_price(b, i);
	if (is_market)
		return 1;
	return s == BOOK_BUY ? lp <= px : lp >= px;
}

/* Total quantity an order could fill right now (for FOK). */
static int64_t
fillable(const book_t *b, int s, int64_t px, int is_market, int64_t want,
    uint32_t account, int stp)
{
	int os = !s;
	uint32_t i = side_best(b, os);
	int64_t got = 0;
	while (i != BOOK_NONE && got < want && crosses(b, s, px, i, is_market)) {
		uint32_t ix = lv_cat(b, os, i)->head;
		/* Hidden iceberg reserve trades too, so count leaves. */
		for (; ix != BOOK_NONE && got < want; ix = b->pool[ix].next) {
			const book_slot_t *m = &b->pool[ix];
			if (stp != BOOK_STP_ALLOW && account != 0 &&
			    m->account == account)
				continue;
			got += m->leaves;
		}
		i = side_next(b, os, i);
	}
	return got;
}

static void trigger_stops(book_t *b, int64_t now_ns, book_out_t *out);

/* Print one fill. */
static void
trade(book_t *b, book_out_t *out, const book_order_t *t, int64_t tleaves,
    book_slot_t *m, int64_t qty, int64_t price)
{
	book_event_t *e = emit(b, out, BOOK_EV_TRADE);
	e->side = t->side;
	e->id = t->id;
	e->other = m->id;
	e->price = price;
	e->qty = qty;
	e->account = t->account;
	e->maker_account = m->account;
	(void)tleaves;
	b->last_price = price;
	b->trades++;
	b->volume += qty;
}

/*
 * Match an incoming order against the opposite side.  Returns the
 * quantity left.  `killed` is set if self-trade prevention cancelled
 * the incoming order.
 */
static int64_t
match(book_t *b, const book_order_t *t, int64_t px, int is_market,
    int64_t leaves, int *killed, book_out_t *out)
{
	int s = t->side, os = !s;
	uint32_t i = side_best(b, os);

	*killed = 0;
	while (leaves > 0 && i != BOOK_NONE &&
	    crosses(b, s, px, i, is_market)) {
		book_level_t *lv = lv_at(b, os, i);
		uint32_t next_level = side_next(b, os, i);
		int touched = 0;

		if (b->cfg.pro_rata && lv->count > 1) {
			/* Pro-rata: each resting order gets leaves * shown /
			 * level qty, rounded down; the remainder goes FIFO. */
			int64_t total = lv->qty, take = leaves < total ?
			    leaves : total, alloc_sum = 0;
			uint32_t ix;
			for (ix = lv->head; ix != BOOK_NONE; ix = b->pool[ix].next) {
				book_slot_t *m = &b->pool[ix];
				int64_t a = take * m->shown / total;
				m->trail = a;		/* scratch: allocation */
				alloc_sum += a;
			}
			for (ix = lv->head; ix != BOOK_NONE &&
			    alloc_sum < take; ix = b->pool[ix].next) {
				book_slot_t *m = &b->pool[ix];
				if (m->trail < m->shown) {
					m->trail++;
					alloc_sum++;
				}
			}
			ix = lv->head;
			while (ix != BOOK_NONE) {
				book_slot_t *m = &b->pool[ix];
				uint32_t nx = m->next;
				int64_t q = m->trail;
				m->trail = 0;
				if (q > 0) {
					uint64_t partner = m->oco;
					trade(b, out, t, leaves, m, q,
					    lv_price(b, i));
					leaves -= q;
					m->leaves -= q;
					m->shown -= q;
					lv->qty -= q;
					touched = 1;
					if (m->leaves == 0) {
						m->live = 0;
						level_unlink(b, os, i, ix);
						release(b, ix);
						oco_kill(b, partner, out);
					}
				}
				ix = nx;
			}
		} else {
			while (leaves > 0 && lv->count > 0) {
				uint32_t ix = lv->head;
				book_slot_t *m = &b->pool[ix];
				int64_t q;

				if (t->stp != BOOK_STP_ALLOW && t->account != 0 &&
				    m->account == t->account) {
					switch (t->stp) {
					case BOOK_STP_CANCEL_OLDEST:
						cancel_ix(b, ix, BOOK_R_SELF_TRADE,
						    out);
						touched = 1;
						continue;
					case BOOK_STP_CANCEL_BOTH:
						cancel_ix(b, ix, BOOK_R_SELF_TRADE,
						    out);
						*killed = 1;
						return leaves;
					case BOOK_STP_DECREMENT: {
						int64_t d = leaves < m->leaves ?
						    leaves : m->leaves;
						leaves -= d;
						if (d >= m->leaves) {
							cancel_ix(b, ix,
							    BOOK_R_SELF_TRADE, out);
						} else {
							int64_t was = m->shown;
							m->leaves -= d;
							if (m->shown > m->leaves)
								m->shown = m->leaves;
							lv->qty -= was - m->shown;
							emit_level(b, out, os, i);
						}
						touched = 1;
						if (leaves == 0) {
							*killed = 1;
							return 0;
						}
						continue;
					}
					case BOOK_STP_CANCEL_NEWEST:
					default:
						*killed = 1;
						if (touched)
							emit_level(b, out, os, i);
						return leaves;
					}
				}
				q = leaves < m->shown ? leaves : m->shown;
				trade(b, out, t, leaves, m, q, lv_price(b, i));
				leaves -= q;
				m->leaves -= q;
				m->shown -= q;
				lv->qty -= q;
				touched = 1;
				if (m->leaves == 0) {
					uint64_t partner = m->oco;
					m->live = 0;
					level_unlink(b, os, i, ix);
					release(b, ix);
					oco_kill(b, partner, out);
				} else if (m->shown == 0) {
					/* Iceberg: refill the peak and go to the
					 * back of the queue at this price. */
					level_requeue(b, os, i, ix, peak(m));
				}
			}
		}
		if (touched)
			emit_level(b, out, os, i);
		i = next_level;
	}
	return leaves;
}

/* ---- submit -------------------------------------------------------- */

static int64_t
peg_price(const book_t *b, const book_order_t *o)
{
	int64_t bid = book_best(b, BOOK_BUY), ask = book_best(b, BOOK_SELL);
	int64_t ref;
	switch (o->peg) {
	case BOOK_PEG_BID:
		ref = bid;
		break;
	case BOOK_PEG_ASK:
		ref = ask;
		break;
	default:
		if (bid == BOOK_NO_PRICE || ask == BOOK_NO_PRICE)
			return BOOK_NO_PRICE;
		ref = bid + (ask - bid) / 2 / b->cfg.tick * b->cfg.tick;
		break;
	}
	if (ref == BOOK_NO_PRICE)
		return BOOK_NO_PRICE;
	return ref + o->peg_offset * b->cfg.tick;
}

/* Enter a limit order that has a price (rest the remainder). */
static void
enter_limit(book_t *b, const book_order_t *o, int64_t price, int64_t now_ns,
    book_out_t *out)
{
	int64_t leaves = o->qty;
	int killed = 0;
	uint32_t ix, li;
	book_slot_t *s;

	(void)now_ns;
	if (!valid_price(b, price)) {
		emit_reject(b, out, o, BOOK_R_BAD_PRICE);
		return;
	}
	if (outside_band(b, price)) {
		emit_reject(b, out, o, BOOK_R_BAND);
		return;
	}
	if (b->phase == BOOK_CONTINUOUS) {
		uint32_t opp = side_best(b, !o->side);
		int cross = opp != BOOK_NONE &&
		    crosses(b, o->side, price, opp, 0);
		if (cross && (o->flags & BOOK_F_POST_ONLY)) {
			emit_reject(b, out, o, BOOK_R_POST_ONLY_CROSS);
			return;
		}
		if (o->tif == BOOK_FOK &&
		    fillable(b, o->side, price, 0, leaves, o->account, o->stp) <
		    leaves) {
			emit_reject(b, out, o, BOOK_R_FOK_UNFILLABLE);
			return;
		}
		if (cross)
			leaves = match(b, o, price, 0, leaves, &killed, out);
	}
	if (killed && leaves > 0) {
		book_event_t *e = emit(b, out, BOOK_EV_CANCELED);
		e->side = o->side;
		e->id = o->id;
		e->price = price;
		e->qty = leaves;
		e->account = o->account;
		e->reason = BOOK_R_SELF_TRADE;
		goto done;
	}
	if (leaves == 0)
		goto done;
	if (o->tif == BOOK_IOC || o->tif == BOOK_FOK) {
		book_event_t *e = emit(b, out, BOOK_EV_CANCELED);
		e->side = o->side;
		e->id = o->id;
		e->price = price;
		e->qty = leaves;
		e->account = o->account;
		e->reason = BOOK_R_IOC;
		goto done;
	}
	ix = pool_get(b);
	if (ix == BOOK_NONE || map_put(b, o->id, ix) != 0) {
		if (ix != BOOK_NONE)
			pool_put(b, ix);
		emit_reject(b, out, o, BOOK_R_FULL);
		goto done;
	}
	s = &b->pool[ix];
	s->id = o->id;
	s->oco = o->oco;
	s->price = price;
	s->leaves = leaves;
	s->display = (o->display > 0 && o->display < leaves) ? o->display : 0;
	s->shown = s->display ? s->display : leaves;
	s->expire_ns = o->expire_ns;
	s->account = o->account;
	s->side = o->side;
	s->type = BOOK_LIMIT;
	s->tif = o->tif;
	s->stp = o->stp;
	s->live = 1;
	li = lv_get(b, o->side, price);
	level_push(b, o->side, li, ix);
	emit_order(b, out, BOOK_EV_ACCEPTED, s, BOOK_R_NONE);
	emit_level(b, out, o->side, li);
done:
	if (b->trades && b->phase == BOOK_CONTINUOUS)
		trigger_stops(b, now_ns, out);
}

static void
enter_market(book_t *b, const book_order_t *o, int64_t now_ns,
    book_out_t *out)
{
	int64_t leaves = o->qty;
	int killed = 0;

	if (b->phase != BOOK_CONTINUOUS) {
		emit_reject(b, out, o, BOOK_R_PHASE);
		return;
	}
	if (side_best(b, !o->side) == BOOK_NONE) {
		emit_reject(b, out, o, BOOK_R_NO_LIQUIDITY);
		return;
	}
	if (o->tif == BOOK_FOK &&
	    fillable(b, o->side, 0, 1, leaves, o->account, o->stp) < leaves) {
		emit_reject(b, out, o, BOOK_R_FOK_UNFILLABLE);
		return;
	}
	leaves = match(b, o, 0, 1, leaves, &killed, out);
	if (leaves > 0) {
		/* A market order never rests: the rest is cancelled. */
		book_event_t *e = emit(b, out, BOOK_EV_CANCELED);
		e->side = o->side;
		e->id = o->id;
		e->qty = leaves;
		e->account = o->account;
		e->reason = killed ? BOOK_R_SELF_TRADE : BOOK_R_IOC;
	}
	trigger_stops(b, now_ns, out);
}

static void
enter_stop(book_t *b, const book_order_t *o, book_out_t *out)
{
	uint32_t ix = pool_get(b);
	book_slot_t *s;

	if (ix == BOOK_NONE || map_put(b, o->id, ix) != 0) {
		if (ix != BOOK_NONE)
			pool_put(b, ix);
		emit_reject(b, out, o, BOOK_R_FULL);
		return;
	}
	s = &b->pool[ix];
	s->id = o->id;
	s->oco = o->oco;
	s->price = o->price;
	s->leaves = o->qty;
	s->shown = o->qty;
	s->display = 0;
	s->expire_ns = o->expire_ns;
	s->account = o->account;
	s->side = o->side;
	s->type = o->type;
	s->tif = o->tif;
	s->stp = o->stp;
	s->live = 2;
	if (o->type == BOOK_TRAILING_STOP) {
		int64_t ref = b->last_price != BOOK_NO_PRICE ? b->last_price :
		    b->ref;
		s->trail = o->stop;
		s->stop = o->side == BOOK_BUY ? ref + o->stop : ref - o->stop;
	} else
		s->stop = o->stop;
	stop_insert(b, o->side, ix);
	emit_order(b, out, BOOK_EV_ACCEPTED, s, BOOK_R_NONE);
}

/* Fire every stop the last trade crossed, repeatedly (a triggered stop
 * can print a trade that triggers the next).  Trailing stops follow the
 * price first. */
static void
trigger_stops(book_t *b, int64_t now_ns, book_out_t *out)
{
	int guard = 0;

	if (b->last_price == BOOK_NO_PRICE)
		return;
	for (;;) {
		int s, fired = 0;
		int64_t lp = b->last_price;

		/* Trailing: a buy stop's trigger falls with the price, a sell
		 * stop's rises; re-sort by re-inserting what moved. */
		for (s = 0; s < 2; s++) {
			uint32_t i;
			for (i = 0; i < b->n_stops[s]; i++) {
				book_slot_t *o = &b->pool[b->stops[s][i]];
				int64_t t;
				if (o->type != BOOK_TRAILING_STOP)
					continue;
				t = s == BOOK_BUY ? lp + o->trail : lp - o->trail;
				if ((s == BOOK_BUY && t < o->stop) ||
				    (s == BOOK_SELL && t > o->stop)) {
					uint32_t ix = b->stops[s][i];
					stop_remove(b, s, ix);
					o->stop = t;
					stop_insert(b, s, ix);
				}
			}
		}
		for (s = 0; s < 2; s++) {
			while (b->n_stops[s] > 0) {
				uint32_t ix = b->stops[s][0];
				book_slot_t *o = &b->pool[ix];
				book_order_t no;
				book_event_t *e;

				if (s == BOOK_BUY ? lp < o->stop : lp > o->stop)
					break;
				stop_remove(b, s, ix);
				memset(&no, 0, sizeof no);
				no.id = o->id;
				no.account = o->account;
				no.side = o->side;
				no.tif = o->tif;
				no.stp = o->stp;
				no.qty = o->leaves;
				no.oco = 0;
				no.expire_ns = o->expire_ns;
				no.type = o->type == BOOK_STOP_LIMIT ? BOOK_LIMIT :
				    BOOK_MARKET;
				no.price = o->price;
				e = emit(b, out, BOOK_EV_TRIGGERED);
				e->side = o->side;
				e->id = o->id;
				e->price = o->stop;
				e->qty = o->leaves;
				e->account = o->account;
				/* OCO: a triggered leg cancels its partner. */
				{
					uint64_t partner = o->oco;
					o->live = 0;
					release(b, ix);
					oco_kill(b, partner, out);
				}
				if (no.type == BOOK_LIMIT)
					enter_limit(b, &no, no.price, now_ns, out);
				else if (side_best(b, !no.side) != BOOK_NONE) {
					int killed;
					int64_t l = match(b, &no, 0, 1, no.qty,
					    &killed, out);
					if (l > 0) {
						e = emit(b, out, BOOK_EV_CANCELED);
						e->side = no.side;
						e->id = no.id;
						e->qty = l;
						e->account = no.account;
						e->reason = BOOK_R_IOC;
					}
				} else {
					e = emit(b, out, BOOK_EV_CANCELED);
					e->side = no.side;
					e->id = no.id;
					e->qty = no.qty;
					e->account = no.account;
					e->reason = BOOK_R_NO_LIQUIDITY;
				}
				fired = 1;
				if (b->last_price != lp)
					break;	/* re-scan with the new price */
			}
		}
		if (!fired || ++guard > 64)
			return;
	}
}

static void
submit(book_t *b, const book_order_t *o, int64_t now_ns, book_out_t *out)
{
	int64_t px;

	if (o->qty <= 0) {
		emit_reject(b, out, o, BOOK_R_BAD_QTY);
		return;
	}
	if (o->id == 0 || map_get(b, o->id) != BOOK_NONE) {
		emit_reject(b, out, o, BOOK_R_DUPLICATE_ID);
		return;
	}
	if (b->phase == BOOK_HALTED || b->phase == BOOK_CANCEL_ONLY ||
	    b->phase == BOOK_CLOSED) {
		emit_reject(b, out, o, BOOK_R_PHASE);
		return;
	}
	if (o->tif == BOOK_GTD && o->expire_ns <= now_ns) {
		emit_reject(b, out, o, BOOK_R_EXPIRED);
		return;
	}
	switch (o->type) {
	case BOOK_LIMIT:
		enter_limit(b, o, o->price, now_ns, out);
		break;
	case BOOK_MARKET:
		enter_market(b, o, now_ns, out);
		break;
	case BOOK_PEGGED:
		px = peg_price(b, o);
		if (px == BOOK_NO_PRICE) {
			emit_reject(b, out, o, BOOK_R_NO_LIQUIDITY);
			return;
		}
		enter_limit(b, o, px, now_ns, out);
		break;
	case BOOK_STOP:
	case BOOK_STOP_LIMIT:
	case BOOK_TRAILING_STOP:
		if (b->phase != BOOK_CONTINUOUS) {
			emit_reject(b, out, o, BOOK_R_PHASE);
			return;
		}
		if (o->type == BOOK_STOP_LIMIT && !valid_price(b, o->price)) {
			emit_reject(b, out, o, BOOK_R_BAD_PRICE);
			return;
		}
		if (o->type == BOOK_TRAILING_STOP && o->stop <= 0) {
			emit_reject(b, out, o, BOOK_R_BAD_PRICE);
			return;
		}
		enter_stop(b, o, out);
		trigger_stops(b, now_ns, out);
		break;
	default:
		emit_reject(b, out, o, BOOK_R_BAD_PRICE);
		break;
	}
}

void
book_submit(book_t *b, const book_order_t *o, int64_t now_ns, book_out_t *out)
{
	submit(b, o, now_ns, out);
	settle(b);
}

/* ---- amend --------------------------------------------------------- */

static uint32_t
owned(const book_t *b, uint64_t id, uint32_t account)
{
	uint32_t ix = map_get(b, id);
	if (ix == BOOK_NONE || !b->pool[ix].live)
		return BOOK_NONE;
	if (account != 0 && b->pool[ix].account != account)
		return BOOK_NONE;
	return ix;
}

void
book_cancel(book_t *b, uint64_t id, uint32_t account, book_out_t *out)
{
	uint32_t ix = owned(b, id, account);
	if (ix == BOOK_NONE) {
		book_event_t *e = emit(b, out, BOOK_EV_REJECTED);
		e->id = id;
		e->account = account;
		e->reason = BOOK_R_UNKNOWN_ID;
		return;
	}
	cancel_ix(b, ix, BOOK_R_USER, out);
	settle(b);
}

void
book_reduce(book_t *b, uint64_t id, uint32_t account, int64_t new_qty,
    book_out_t *out)
{
	uint32_t ix = owned(b, id, account);
	book_slot_t *o;

	if (ix == BOOK_NONE) {
		book_event_t *e = emit(b, out, BOOK_EV_REJECTED);
		e->id = id;
		e->account = account;
		e->reason = BOOK_R_UNKNOWN_ID;
		return;
	}
	o = &b->pool[ix];
	if (new_qty <= 0) {
		cancel_ix(b, ix, BOOK_R_USER, out);
		settle(b);
		return;
	}
	if (new_qty >= o->leaves) {
		book_event_t *e = emit(b, out, BOOK_EV_REJECTED);
		e->id = id;
		e->account = account;
		e->qty = new_qty;
		e->reason = BOOK_R_BAD_QTY;
		return;
	}
	if (o->live == 1) {
		uint32_t li = lv_find(b, o->side, o->price);
		book_level_t *lv = lv_at(b, o->side, li);
		int64_t was = o->shown;
		o->leaves = new_qty;
		if (o->shown > new_qty)
			o->shown = new_qty;
		lv->qty -= was - o->shown;
		emit_order(b, out, BOOK_EV_REDUCED, o, BOOK_R_NONE);
		emit_level(b, out, o->side, li);
	} else {
		o->leaves = new_qty;
		o->shown = new_qty;
		emit_order(b, out, BOOK_EV_REDUCED, o, BOOK_R_NONE);
	}
}

void
book_replace(book_t *b, uint64_t id, uint32_t account, uint64_t new_id,
    int64_t price, int64_t qty, int64_t now_ns, book_out_t *out)
{
	uint32_t ix = owned(b, id, account);
	book_order_t no;
	book_slot_t *o;
	book_event_t *e;

	if (ix == BOOK_NONE) {
		e = emit(b, out, BOOK_EV_REJECTED);
		e->id = id;
		e->account = account;
		e->reason = BOOK_R_UNKNOWN_ID;
		return;
	}
	o = &b->pool[ix];
	if (o->live != 1 || qty <= 0 || new_id == 0 ||
	    (new_id != id && map_get(b, new_id) != BOOK_NONE)) {
		e = emit(b, out, BOOK_EV_REJECTED);
		e->id = id;
		e->account = account;
		e->reason = qty <= 0 ? BOOK_R_BAD_QTY : BOOK_R_DUPLICATE_ID;
		return;
	}
	memset(&no, 0, sizeof no);
	no.id = new_id;
	no.account = o->account;
	no.side = o->side;
	no.type = BOOK_LIMIT;
	no.tif = o->tif;
	no.stp = o->stp;
	no.price = price;
	no.qty = qty;
	no.display = o->display;
	no.expire_ns = o->expire_ns;
	e = emit(b, out, BOOK_EV_REPLACED);
	e->side = o->side;
	e->id = id;
	e->other = new_id;
	e->price = price;
	e->qty = qty;
	e->account = o->account;
	rest_remove(b, ix, out);
	o->live = 0;
	release(b, ix);
	enter_limit(b, &no, price, now_ns, out);
	settle(b);
}

uint32_t
book_cancel_all(book_t *b, uint32_t account, book_out_t *out)
{
	uint32_t ix, n = 0;
	for (ix = 0; ix < b->cfg.max_orders; ix++)
		if (b->pool[ix].live && b->pool[ix].account == account) {
			cancel_ix(b, ix, BOOK_R_USER, out);
			n++;
		}
	settle(b);
	return n;
}

uint32_t
book_expire(book_t *b, int64_t now_ns, book_out_t *out)
{
	uint32_t ix, n = 0;
	for (ix = 0; ix < b->cfg.max_orders; ix++) {
		book_slot_t *o = &b->pool[ix];
		if (!o->live)
			continue;
		if ((o->tif == BOOK_GTD && o->expire_ns <= now_ns) ||
		    (o->tif == BOOK_DAY && b->phase == BOOK_CLOSED)) {
			cancel_ix(b, ix, BOOK_R_EXPIRED, out);
			n++;
		}
	}
	settle(b);
	return n;
}

/* ---- auction ------------------------------------------------------- */

/* Volume executable at price p: min(bids >= p, asks <= p). */
static void
auction_at(const book_t *b, int64_t p, int64_t *exec, int64_t *imb)
{
	int64_t q[2] = { 0, 0 };
	int s;
	for (s = 0; s < 2; s++) {
		uint32_t j, ix;
		for (j = side_best(b, s);
		     j != BOOK_NONE && !worse(s, lv_price(b, j), p);
		     j = side_next(b, s, j))
			for (ix = lv_cat(b, s, j)->head; ix != BOOK_NONE;
			     ix = b->pool[ix].next)
				q[s] += b->pool[ix].leaves;
	}
	*exec = q[0] < q[1] ? q[0] : q[1];
	*imb = q[0] > q[1] ? q[0] - q[1] : q[1] - q[0];
}

typedef struct {
	int64_t price, exec, imb, dist;
} auc_best_t;

/*
 * Consider clearing price p in [lo, hi].  The order is total: maximum
 * volume, then minimum imbalance, then nearest the reference, then the
 * lower price.  The volume and imbalance are constant between level
 * prices, so the best price is one of: the crossed range's ends, each
 * level's price and its neighbour tick, and the ticks either side of
 * the reference.  The caller offers exactly those.
 */
static void
auction_try(const book_t *b, int64_t p, int64_t lo, int64_t hi,
    auc_best_t *best)
{
	int64_t ex, imb, d;
	if (p < lo || p > hi)
		return;
	auction_at(b, p, &ex, &imb);
	d = dist(p, b->ref);
	if (ex == 0 || ex < best->exec)
		return;
	if (best->exec == ex) {
		if (imb != best->imb) {
			if (imb > best->imb)
				return;
		} else if (d != best->dist) {
			if (d > best->dist)
				return;
		} else if (p >= best->price)
			return;
	}
	best->price = p;
	best->exec = ex;
	best->imb = imb;
	best->dist = d;
}

int64_t
book_auction(book_t *b, book_out_t *out)
{
	int64_t lo = book_best(b, BOOK_SELL), hi = book_best(b, BOOK_BUY);
	int64_t t = b->cfg.tick, fr, crossed = 0;
	uint8_t saved = b->phase;
	auc_best_t best;
	book_order_t o;
	uint32_t j;
	int s;

	if (lo == BOOK_NO_PRICE || hi == BOOK_NO_PRICE || lo > hi)
		return 0;		/* not crossed */
	memset(&best, 0, sizeof best);
	auction_try(b, lo, lo, hi, &best);
	auction_try(b, hi, lo, hi, &best);
	fr = b->ref - b->ref % t;
	auction_try(b, fr, lo, hi, &best);
	auction_try(b, fr + t, lo, hi, &best);
	for (s = 0; s < 2; s++)
		for (j = side_best(b, s); j != BOOK_NONE &&
		     lv_price(b, j) >= lo && lv_price(b, j) <= hi;
		     j = side_next(b, s, j)) {
			auction_try(b, lv_price(b, j), lo, hi, &best);
			auction_try(b,
			    lv_price(b, j) + (s == BOOK_BUY ? t : -t), lo, hi,
			    &best);
		}
	if (best.exec == 0)
		return 0;
	/* Execute: walk the bids in priority order, each one matched as a
	 * limit at the clearing price, printing at that single price. */
	b->phase = BOOK_CONTINUOUS;
	while (crossed < best.exec) {
		uint32_t li[2], ixs[2];
		book_slot_t *bo, *ao;
		int64_t q;
		int k;
		li[0] = side_best(b, BOOK_BUY);
		li[1] = side_best(b, BOOK_SELL);
		if (li[0] == BOOK_NONE || li[1] == BOOK_NONE ||
		    lv_price(b, li[0]) < best.price ||
		    lv_price(b, li[1]) > best.price)
			break;
		ixs[0] = lv_cat(b, 0, li[0])->head;
		ixs[1] = lv_cat(b, 1, li[1])->head;
		bo = &b->pool[ixs[0]];
		ao = &b->pool[ixs[1]];
		q = bo->shown < ao->shown ? bo->shown : ao->shown;
		if (q > best.exec - crossed)
			q = best.exec - crossed;
		memset(&o, 0, sizeof o);
		o.id = bo->id;
		o.account = bo->account;
		o.side = BOOK_BUY;
		trade(b, out, &o, bo->leaves, ao, q, best.price);
		crossed += q;
		for (k = 0; k < 2; k++) {
			book_slot_t *m = &b->pool[ixs[k]];
			m->leaves -= q;
			m->shown -= q;
			lv_at(b, k, li[k])->qty -= q;
			if (m->leaves == 0) {
				uint64_t partner = m->oco;
				m->live = 0;
				level_unlink(b, k, li[k], ixs[k]);
				release(b, ixs[k]);
				oco_kill(b, partner, out);
			} else if (m->shown == 0)
				level_requeue(b, k, li[k], ixs[k], peak(m));
			emit_level(b, out, k, li[k]);
		}
	}
	b->phase = saved;
	b->ref = best.price;
	settle(b);
	return crossed;
}

void
book_set_phase(book_t *b, book_phase_t p, book_out_t *out)
{
	book_event_t *e;
	if (b->phase == BOOK_PREOPEN && p == BOOK_CONTINUOUS)
		(void)book_auction(b, out);
	b->phase = (uint8_t)p;
	e = emit(b, out, BOOK_EV_PHASE);
	e->qty = p;
	if (p == BOOK_CLOSED)
		(void)book_expire(b, INT64_MIN, out);
}

/* ---- queries ------------------------------------------------------- */

int64_t
book_best(const book_t *b, book_side_t s)
{
	uint32_t i = side_best(b, s);
	return i == BOOK_NONE ? BOOK_NO_PRICE : lv_price(b, i);
}

int64_t
book_level_qty(const book_t *b, book_side_t s, int64_t price)
{
	uint32_t h;
	if (!valid_price(b, price))
		return 0;
	h = lv_find(b, s, price);
	return h == BOOK_NONE ? 0 : lv_cat(b, s, h)->qty;
}

uint32_t
book_depth(const book_t *b, book_side_t s, uint32_t depth, int64_t *price,
    int64_t *qty)
{
	uint32_t i = side_best(b, s), n = 0;
	while (i != BOOK_NONE && n < depth) {
		price[n] = lv_price(b, i);
		qty[n] = lv_cat(b, s, i)->qty;
		n++;
		i = side_next(b, s, i);
	}
	return n;
}

const book_slot_t *
book_find(const book_t *b, uint64_t id)
{
	uint32_t ix = map_get(b, id);
	return ix == BOOK_NONE ? NULL : &b->pool[ix];
}

static uint64_t
fnv(uint64_t h, const void *p, size_t n)
{
	const unsigned char *c = p;
	while (n--) {
		h ^= *c++;
		h *= 0x100000001b3ull;
	}
	return h;
}

void
book_foreach_resting(const book_t *b,
    void (*fn)(const book_slot_t *o, void *arg), void *arg)
{
	int s;
	for (s = 0; s < 2; s++) {
		uint32_t i = side_best(b, s);
		while (i != BOOK_NONE) {
			uint32_t ix;
			for (ix = lv_cat(b, s, i)->head; ix != BOOK_NONE;
			     ix = b->pool[ix].next)
				fn(&b->pool[ix], arg);
			i = side_next(b, s, i);
		}
	}
}

uint64_t
book_digest(const book_t *b)
{
	uint64_t h = 0xcbf29ce484222325ull;
	int s;
	h = fnv(h, &b->seq, sizeof b->seq);
	h = fnv(h, &b->last_price, sizeof b->last_price);
	for (s = 0; s < 2; s++) {
		uint32_t i = side_best(b, s), k;
		while (i != BOOK_NONE) {
			uint32_t ix;
			int64_t p = lv_price(b, i);
			h = fnv(h, &p, sizeof p);
			for (ix = lv_cat(b, s, i)->head; ix != BOOK_NONE;
			     ix = b->pool[ix].next) {
				const book_slot_t *o = &b->pool[ix];
				h = fnv(h, &o->id, sizeof o->id);
				h = fnv(h, &o->leaves, sizeof o->leaves);
				h = fnv(h, &o->shown, sizeof o->shown);
			}
			i = side_next(b, s, i);
		}
		for (k = 0; k < b->n_stops[s]; k++) {
			const book_slot_t *o = &b->pool[b->stops[s][k]];
			h = fnv(h, &o->id, sizeof o->id);
			h = fnv(h, &o->stop, sizeof o->stop);
		}
	}
	return h;
}

/* The FIFO of one level: links, owners, totals.  0 = sound. */
static int
check_level(const book_t *b, int s, const book_level_t *lv, int64_t price)
{
	int64_t q = 0;
	uint32_t c = 0, ix, prev = BOOK_NONE;
	for (ix = lv->head; ix != BOOK_NONE; ix = b->pool[ix].next) {
		const book_slot_t *o = &b->pool[ix];
		if (ix >= b->cfg.max_orders || c >= b->cfg.max_orders ||
		    o->prev != prev || o->live != 1 || o->side != s ||
		    o->price != price || o->shown <= 0 || o->shown > o->leaves)
			return 3;
		if (map_get(b, o->id) != ix)
			return 4;
		q += o->shown;
		c++;
		prev = ix;
	}
	if (lv->tail != prev || q != lv->qty || c != lv->count)
		return 5;
	return 0;
}

int
book_check(const book_t *b)
{
	int s, rc;
	uint32_t live = 0, k, x, nfree = 0;
	int64_t nb;

	if (b->base < b->cfg.tick || b->base % b->cfg.tick != 0)
		return 10;
	for (s = 0; s < 2; s++) {
		uint32_t i, nl = 0;
		for (i = 0; i < BOOK_LADDER; i++) {
			const book_level_t *lv = &b->side[s].lv[i];
			int set = (b->side[s].bits[i >> 6] >> (i & 63)) & 1;
			if (set != (lv->count > 0))
				return 1;
			if (((b->side[s].summary >> (i >> 6)) & 1) !=
			    (b->side[s].bits[i >> 6] != 0))
				return 2;
			if ((rc = check_level(b, s, lv, lv_price(b, i))) != 0)
				return rc;
			if (lv->count)
				nl++;
			live += lv->count;
		}
		if (nl != b->side[s].nlevels)
			return 6;
		/* Overflow: sorted worst first, outside the window, whole
		 * ticks, no empty level. */
		if (b->nx[s] > b->cfg.max_orders)
			return 10;
		for (k = 0; k < b->nx[s]; k++) {
			const book_xlevel_t *xl;
			x = b->xord[s][k];
			if (x >= b->cfg.max_orders)
				return 10;
			xl = &b->xlv[x];
			if (in_win(b, xl->price) ||
			    !valid_price(b, xl->price) || xl->lv.count == 0 ||
			    (k > 0 &&
			        !worse(s, b->xlv[b->xord[s][k - 1]].price,
			            xl->price)))
				return 10;
			if ((rc = check_level(b, s, &xl->lv, xl->price)) != 0)
				return rc;
			live += xl->lv.count;
		}
		live += b->n_stops[s];
	}
	if (live != b->n_live)
		return 7;
	/* Every live order has one id-map entry and nothing else does. */
	if (b->map_used != b->n_live)
		return 9;
	/* Every overflow slot is in a side's list or on the free list. */
	for (x = b->xfree; x != BOOK_NONE; x = b->xlv[x].lv.xnext)
		if (x >= b->cfg.max_orders || ++nfree > b->cfg.max_orders)
			return 10;
	if (nfree + b->nx[0] + b->nx[1] != b->cfg.max_orders)
		return 10;
	/* The window is where settle() puts it. */
	if (settle_base(b, &nb))
		return 11;
	/* Uncrossed in continuous trading. */
	if (b->phase == BOOK_CONTINUOUS) {
		int64_t bid = book_best(b, BOOK_BUY), ask = book_best(b, BOOK_SELL);
		if (bid != BOOK_NO_PRICE && ask != BOOK_NO_PRICE && bid >= ask)
			return 8;
	}
	return 0;
}

/* ---- init ---------------------------------------------------------- */

static uint32_t
pow2_at_least(uint32_t x)
{
	uint32_t p = 1;
	while (p < x)
		p <<= 1;
	return p;
}

static size_t
align64(size_t x)
{
	return (x + 63) & ~(size_t)63;
}

size_t
book_bytes(const book_config_t *cfg)
{
	uint32_t mc;
	if (cfg == NULL || cfg->max_orders == 0)
		return 0;
	mc = pow2_at_least(cfg->max_orders * 2);
	/* book, pool, id map, two stop arrays, and the overflow store:
	 * max_orders level slots and two sorted slot arrays. */
	return align64(sizeof(book_t)) +
	    align64((size_t)cfg->max_orders * sizeof(book_slot_t)) +
	    align64((size_t)mc * sizeof(uint64_t)) +
	    align64((size_t)mc * sizeof(uint32_t)) +
	    2 * align64((size_t)cfg->max_orders * sizeof(uint32_t)) +
	    align64((size_t)cfg->max_orders * sizeof(book_xlevel_t)) +
	    2 * align64((size_t)cfg->max_orders * sizeof(uint32_t));
}

int
book_init(book_t **out, void *mem, size_t len, const book_config_t *cfg)
{
	unsigned char *p = mem;
	book_t *b;
	uint32_t i, mc;
	int s;

	if (out == NULL || mem == NULL || cfg == NULL || cfg->max_orders == 0 ||
	    cfg->max_orders >= BOOK_NONE / 2 || cfg->tick <= 0 ||
	    cfg->tick > BOOK_PRICE_MAX / BOOK_LADDER || cfg->ref_price <= 0 ||
	    cfg->ref_price > BOOK_PRICE_MAX || len < book_bytes(cfg) ||
	    ((uintptr_t)mem & 63) != 0)
		return -1;
	memset(mem, 0, book_bytes(cfg));
	mc = pow2_at_least(cfg->max_orders * 2);
	b = (book_t *)p;
	p += align64(sizeof(book_t));
	b->cfg = *cfg;
	b->pool = (book_slot_t *)p;
	p += align64((size_t)cfg->max_orders * sizeof(book_slot_t));
	b->map_id = (uint64_t *)p;
	p += align64((size_t)mc * sizeof(uint64_t));
	b->map_ix = (uint32_t *)p;
	p += align64((size_t)mc * sizeof(uint32_t));
	b->stops[0] = (uint32_t *)p;
	p += align64((size_t)cfg->max_orders * sizeof(uint32_t));
	b->stops[1] = (uint32_t *)p;
	p += align64((size_t)cfg->max_orders * sizeof(uint32_t));
	b->xlv = (book_xlevel_t *)p;
	p += align64((size_t)cfg->max_orders * sizeof(book_xlevel_t));
	b->xord[0] = (uint32_t *)p;
	p += align64((size_t)cfg->max_orders * sizeof(uint32_t));
	b->xord[1] = (uint32_t *)p;
	b->map_mask = mc - 1;
	for (i = 0; i < cfg->max_orders; i++)
		b->pool[i].next = i + 1 < cfg->max_orders ? i + 1 : BOOK_NONE;
	b->free_head = 0;
	for (i = 0; i < cfg->max_orders; i++) {
		lv_clear(&b->xlv[i].lv);
		b->xlv[i].lv.xnext =
		    i + 1 < cfg->max_orders ? i + 1 : BOOK_NONE;
	}
	b->xfree = 0;
	for (s = 0; s < 2; s++)
		for (i = 0; i < BOOK_LADDER; i++)
			b->side[s].lv[i].head = b->side[s].lv[i].tail = BOOK_NONE;
	/* Centre the window on the reference price, aligned to the tick. */
	b->base = cfg->ref_price - (int64_t)(BOOK_LADDER / 2) * cfg->tick;
	b->base -= b->base % cfg->tick;
	if (b->base < cfg->tick)
		b->base = cfg->tick;
	b->ref = cfg->ref_price;
	b->last_price = BOOK_NO_PRICE;
	b->phase = BOOK_CONTINUOUS;
	*out = b;
	return 0;
}
