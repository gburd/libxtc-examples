/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/book.c -- the matching engine.  See book.h for the shape.
 *
 *	Price-time priority.  A price is an index into a dense ladder of
 *	BOOK_LADDER levels starting at b->base.  Each side keeps a bitmap of
 *	non-empty levels under a one-word summary, so "best bid" is the
 *	highest set bit and "best ask" the lowest, two find-first-set
 *	instructions, and walking the book is a scan of set bits.
 *
 *	A price outside the band is rejected with BOOK_R_BAD_PRICE unless
 *	the band can re-centre on it (only when the book is empty on the
 *	far side of the move).  For US equities a 4096-cent band around the
 *	touch is +-$20 on a penny tick, wider than any LULD limit band, so
 *	re-centring is an edge case, not the hot path.
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

static inline uint32_t
lv_index(const book_t *b, int64_t price)
{
	return (uint32_t)((price - b->base) / b->cfg.tick);
}

static inline int64_t
lv_price(const book_t *b, uint32_t i)
{
	return b->base + (int64_t)i * b->cfg.tick;
}

static inline int
in_band(const book_t *b, int64_t price)
{
	return price >= b->base &&
	    price < b->base + (int64_t)BOOK_LADDER * b->cfg.tick &&
	    (price - b->base) % b->cfg.tick == 0;
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

/* Best level index of a side (bid: highest, ask: lowest). */
static inline uint32_t
side_best(const book_t *b, int s)
{
	return s == BOOK_BUY ? ladder_max(&b->side[0]) :
	    ladder_min(&b->side[1]);
}

/* The next level away from the touch. */
static inline uint32_t
side_next(const book_t *b, int s, uint32_t i)
{
	return s == BOOK_BUY ? ladder_below(&b->side[0], i) :
	    ladder_above(&b->side[1], i);
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
emit_level(book_t *b, book_out_t *out, int s, uint32_t i)
{
	book_event_t *e = emit(b, out, BOOK_EV_LEVEL);
	e->side = (uint8_t)s;
	e->price = lv_price(b, i);
	e->qty = b->side[s].lv[i].qty;
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
#define MAP_TOMB	~0ull

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
	uint32_t h, tomb = BOOK_NONE;
	if (b->map_used + 1 > (b->map_mask + 1) / 4 * 3)
		return -1;
	h = map_hash(id) & b->map_mask;
	for (;;) {
		uint64_t k = b->map_id[h];
		if (k == id)
			return -1;
		if (k == MAP_TOMB && tomb == BOOK_NONE)
			tomb = h;
		if (k == MAP_EMPTY)
			break;
		h = (h + 1) & b->map_mask;
	}
	if (tomb != BOOK_NONE)
		h = tomb;
	else
		b->map_used++;
	b->map_id[h] = id;
	b->map_ix[h] = ix;
	return 0;
}

static void
map_del(book_t *b, uint64_t id)
{
	uint32_t h = map_hash(id) & b->map_mask;
	for (;;) {
		uint64_t k = b->map_id[h];
		if (k == id) {
			/* An empty successor lets this slot be empty too. */
			if (b->map_id[(h + 1) & b->map_mask] == MAP_EMPTY) {
				b->map_id[h] = MAP_EMPTY;
				b->map_used--;
			} else
				b->map_id[h] = MAP_TOMB;
			return;
		}
		if (k == MAP_EMPTY)
			return;
		h = (h + 1) & b->map_mask;
	}
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

/* ---- level queue --------------------------------------------------- */

static void
level_push(book_t *b, int s, uint32_t li, uint32_t ix)
{
	book_level_t *lv = &b->side[s].lv[li];
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
		ladder_set(&b->side[s], li);
		b->side[s].nlevels++;
	}
}

static void
level_unlink(book_t *b, int s, uint32_t li, uint32_t ix)
{
	book_level_t *lv = &b->side[s].lv[li];
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
		ladder_clear(&b->side[s], li);
		b->side[s].nlevels--;
	}
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
	uint32_t li = lv_index(b, o->price);
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
		uint32_t ix = b->side[os].lv[i].head;
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
		book_level_t *lv = &b->side[os].lv[i];
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
					level_unlink(b, os, i, ix);
					m->shown = m->leaves < m->display ?
					    m->leaves : m->display;
					level_push(b, os, i, ix);
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
	if (!in_band(b, price)) {
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
	li = lv_index(b, price);
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

void
book_submit(book_t *b, const book_order_t *o, int64_t now_ns, book_out_t *out)
{
	int64_t px;

	if (o->qty <= 0) {
		emit_reject(b, out, o, BOOK_R_BAD_QTY);
		return;
	}
	if (o->id == 0 || o->id == MAP_TOMB || map_get(b, o->id) != BOOK_NONE) {
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
		if (o->type == BOOK_STOP_LIMIT && !in_band(b, o->price)) {
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
		book_level_t *lv = &b->side[o->side].lv[lv_index(b, o->price)];
		int64_t was = o->shown;
		o->leaves = new_qty;
		if (o->shown > new_qty)
			o->shown = new_qty;
		lv->qty -= was - o->shown;
		emit_order(b, out, BOOK_EV_REDUCED, o, BOOK_R_NONE);
		emit_level(b, out, o->side, lv_index(b, o->price));
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
	return n;
}

/* ---- auction ------------------------------------------------------- */

/* Volume executable at price p: min(bids >= p, asks <= p). */
static void
auction_at(const book_t *b, uint32_t i, int64_t *exec, int64_t *imb)
{
	int64_t bids = 0, asks = 0;
	uint32_t j;
	for (j = ladder_max(&b->side[0]); j != BOOK_NONE && j >= i;
	    j = ladder_below(&b->side[0], j)) {
		uint32_t ix;
		for (ix = b->side[0].lv[j].head; ix != BOOK_NONE;
		    ix = b->pool[ix].next)
			bids += b->pool[ix].leaves;
	}
	for (j = ladder_min(&b->side[1]); j != BOOK_NONE && j <= i;
	    j = ladder_above(&b->side[1], j)) {
		uint32_t ix;
		for (ix = b->side[1].lv[j].head; ix != BOOK_NONE;
		    ix = b->pool[ix].next)
			asks += b->pool[ix].leaves;
	}
	*exec = bids < asks ? bids : asks;
	*imb = bids > asks ? bids - asks : asks - bids;
}

int64_t
book_auction(book_t *b, book_out_t *out)
{
	uint32_t lo = ladder_min(&b->side[1]), hi = ladder_max(&b->side[0]);
	uint32_t i, best = BOOK_NONE;
	int64_t best_exec = 0, best_imb = 0, crossed = 0;
	uint8_t saved = b->phase;
	book_order_t t;

	if (lo == BOOK_NONE || hi == BOOK_NONE || lo > hi)
		return 0;		/* not crossed */
	for (i = lo; i <= hi; i++) {
		int64_t ex, imb, d, bd;
		auction_at(b, i, &ex, &imb);
		if (ex > best_exec || (ex == best_exec && ex > 0 &&
		    (imb < best_imb || (imb == best_imb &&
		    (d = lv_price(b, i) - b->ref, d < 0 ? -d : d) <
		    (bd = lv_price(b, best) - b->ref, bd < 0 ? -bd : bd))))) {
			best = i;
			best_exec = ex;
			best_imb = imb;
		}
	}
	if (best == BOOK_NONE || best_exec == 0)
		return 0;
	/* Execute: walk the bids in priority order, each one matched as a
	 * limit at the clearing price, printing at that single price. */
	b->phase = BOOK_CONTINUOUS;
	while (crossed < best_exec) {
		uint32_t bi = ladder_max(&b->side[0]), ai = ladder_min(&b->side[1]);
		book_slot_t *bo, *ao;
		int64_t q;
		if (bi == BOOK_NONE || ai == BOOK_NONE || bi < best || ai > best)
			break;
		bo = &b->pool[b->side[0].lv[bi].head];
		ao = &b->pool[b->side[1].lv[ai].head];
		q = bo->shown < ao->shown ? bo->shown : ao->shown;
		if (q > best_exec - crossed)
			q = best_exec - crossed;
		memset(&t, 0, sizeof t);
		t.id = bo->id;
		t.account = bo->account;
		t.side = BOOK_BUY;
		trade(b, out, &t, bo->leaves, ao, q, lv_price(b, best));
		crossed += q;
		{
			uint32_t bix = b->side[0].lv[bi].head;
			uint32_t aix = b->side[1].lv[ai].head;
			int k;
			for (k = 0; k < 2; k++) {
				uint32_t ix = k ? aix : bix, li = k ? ai : bi;
				book_slot_t *o = &b->pool[ix];
				o->leaves -= q;
				o->shown -= q;
				b->side[k].lv[li].qty -= q;
				if (o->leaves == 0) {
					uint64_t partner = o->oco;
					o->live = 0;
					level_unlink(b, k, li, ix);
					release(b, ix);
					oco_kill(b, partner, out);
				} else if (o->shown == 0) {
					level_unlink(b, k, li, ix);
					o->shown = o->leaves < o->display ?
					    o->leaves : o->display;
					level_push(b, k, li, ix);
				}
				emit_level(b, out, k, li);
			}
		}
	}
	b->phase = saved;
	b->ref = lv_price(b, best);
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
	if (!in_band(b, price))
		return 0;
	return b->side[s].lv[lv_index(b, price)].qty;
}

uint32_t
book_depth(const book_t *b, book_side_t s, uint32_t depth, int64_t *price,
    int64_t *qty)
{
	uint32_t i = side_best(b, s), n = 0;
	while (i != BOOK_NONE && n < depth) {
		price[n] = lv_price(b, i);
		qty[n] = b->side[s].lv[i].qty;
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
			for (ix = b->side[s].lv[i].head; ix != BOOK_NONE;
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
			for (ix = b->side[s].lv[i].head; ix != BOOK_NONE;
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

int
book_check(const book_t *b)
{
	int s;
	uint32_t live = 0;
	for (s = 0; s < 2; s++) {
		uint32_t i, nl = 0;
		for (i = 0; i < BOOK_LADDER; i++) {
			const book_level_t *lv = &b->side[s].lv[i];
			int set = (b->side[s].bits[i >> 6] >> (i & 63)) & 1;
			int64_t q = 0;
			uint32_t c = 0, ix, prev = BOOK_NONE;
			if (set != (lv->count > 0))
				return 1;
			if (((b->side[s].summary >> (i >> 6)) & 1) !=
			    (b->side[s].bits[i >> 6] != 0))
				return 2;
			for (ix = lv->head; ix != BOOK_NONE; ix = b->pool[ix].next) {
				const book_slot_t *o = &b->pool[ix];
				if (o->prev != prev || o->live != 1 || o->side != s ||
				    lv_index(b, o->price) != i || o->shown <= 0 ||
				    o->shown > o->leaves)
					return 3;
				if (map_get(b, o->id) != ix)
					return 4;
				q += o->shown;
				c++;
				prev = ix;
			}
			if (lv->tail != prev || q != lv->qty || c != lv->count)
				return 5;
			if (lv->count)
				nl++;
			live += c;
		}
		if (nl != b->side[s].nlevels)
			return 6;
		live += b->n_stops[s];
	}
	if (live != b->n_live)
		return 7;
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
	return align64(sizeof(book_t)) +
	    align64((size_t)cfg->max_orders * sizeof(book_slot_t)) +
	    align64((size_t)mc * sizeof(uint64_t)) +
	    align64((size_t)mc * sizeof(uint32_t)) +
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
	    cfg->tick <= 0 || cfg->ref_price <= 0 || len < book_bytes(cfg) ||
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
	b->map_mask = mc - 1;
	for (i = 0; i < cfg->max_orders; i++)
		b->pool[i].next = i + 1 < cfg->max_orders ? i + 1 : BOOK_NONE;
	b->free_head = 0;
	for (s = 0; s < 2; s++)
		for (i = 0; i < BOOK_LADDER; i++)
			b->side[s].lv[i].head = b->side[s].lv[i].tail = BOOK_NONE;
	/* Centre the band on the reference price, aligned to the tick. */
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
