/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/console.c -- the browser console's market.  See console.h.
 *
 *	Each function names the cmd/obwasm (intrepidkarthi/orderbook
 *	3fffc25, MIT) function it reproduces.  What differs is the engine
 *	underneath (book.c) and how state is held (fixed tables, no maps).
 */
#include "console.h"

#if defined(__wasm__) && !defined(__wasi__)
#define memset(d, c, n)		__builtin_memset((d), (c), (n))
#else
#include <string.h>
#endif

/* The console's sim.NoiseTrader (DefaultNoiseTrader, then cmd/obwasm's
 * MinSize, MaxSize = 100, 2000 and MaxOffsetTicks = 6). */
#define NT_POPULATION	50
#define NT_ACT_PROB	0.9
#define NT_MARKET_FRAC	0.3
#define NT_MAX_OFFSET	6
#define NT_MIN_SIZE	100
#define NT_MAX_SIZE	2000

/* The provocations and their detectors (cmd/obwasm constants). */
#define SPOOF_LAYERS	3
#define SPOOF_SIZE	8000
#define SPOOF_MIN_SIZE	5000
#define SPOOF_LIFETIME	500
#define SPOOF_STEPS	3
#define FLOOD_ORDERS	30
#define OTR_WINDOW	600
#define OTR_MIN_ORDERS	25
#define OTR_MAX_RATIO	15.0

size_t
con_book_bytes(void)
{
	book_config_t cfg;

	memset(&cfg, 0, sizeof cfg);
	cfg.max_orders = CON_MAX_ORDERS;
	cfg.tick = 1;
	cfg.ref_price = CON_START;
	return book_bytes(&cfg);
}

/* reset / newConsole: their engine has no price band, so neither does
 * this one: any positive price rests. */
int
con_init(con_t *c, void *mem, size_t len, int64_t seed)
{
	book_config_t cfg;
	sv_config_t sv;

	memset(&cfg, 0, sizeof cfg);
	cfg.max_orders = CON_MAX_ORDERS;
	cfg.tick = 1;
	cfg.ref_price = CON_START;
	memset(c, 0, sizeof *c - sizeof c->ev);
	if (book_init(&c->b, mem, len, &cfg) != 0)
		return -1;
	gorand_seed(&c->rng, seed);
	c->next_id = 1;
	c->out.ev = c->ev;
	c->out.cap = CON_EVENTS;
	memset(&sv, 0, sizeof sv);
	sv.min_size = SPOOF_MIN_SIZE;
	sv.max_lifetime = SPOOF_LIFETIME;
	sv.otr_window = OTR_WINDOW;
	sv.otr_min_orders = OTR_MIN_ORDERS;
	sv.otr_max_ratio = OTR_MAX_RATIO;
	sv_init(&c->sv, &sv);
	return 0;
}

void
con_snapshot(const con_t *c, sig_snap_t *s)
{
	s->nb = book_depth(c->b, BOOK_BUY, CON_DEPTH, s->bp, s->bq);
	s->na = book_depth(c->b, BOOK_SELL, CON_DEPTH, s->ap, s->aq);
}

static void
keep_trade(con_t *c, const book_event_t *e)
{
	con_trade_t *t;

	if (c->n_trades == CON_TRADES) {
		c->trades_more++;
		return;
	}
	t = &c->trades[c->n_trades++];
	t->price = e->price;
	t->qty = e->qty;
	t->taker_sold = e->side == BOOK_SELL;
}

/*
 * engine.Process then observe: surveillance sees the placement and every
 * print, CVD accumulates the trades.  A visitor's order and a noise
 * agent's take the same path.  Returns the reason the engine refused the
 * order, or 0; *flow gets its signed aggressor volume.
 */
static int
process(con_t *c, book_order_t *o, int64_t *flow)
{
	sv_event_t ev;
	uint32_t i;
	int reason = 0;
	const book_slot_t *rest;

	o->id = c->next_id++;
	o->stp = BOOK_STP_CANCEL_NEWEST;	/* their DefaultConfig */
	c->out.n = 0;
	c->out.overflow = 0;
	book_submit(c->b, o, 0, &c->out);
	c->overflow += c->out.overflow;
	memset(&ev, 0, sizeof ev);
	ev.kind = SV_PLACED;
	ev.user = o->account;
	ev.seq = ++c->seq;
	ev.order = o->id;
	ev.qty = o->qty;
	(void)sv_observe(&c->sv, &ev);
	for (i = 0; i < c->out.n; i++) {
		const book_event_t *e = &c->out.ev[i];
		if (e->kind == BOOK_EV_REJECTED && e->id == o->id)
			reason = e->reason;
		if (e->kind != BOOK_EV_TRADE)
			continue;
		/* signals.SignedFlow: +q when the taker bought, -q when it
		 * sold (kyle.go); CVD is its running sum (flow.go). */
		*flow += e->side == BOOK_SELL ? -e->qty : e->qty;
		memset(&ev, 0, sizeof ev);
		ev.kind = SV_TRADE;
		ev.user = e->account;
		ev.seq = ++c->seq;
		ev.maker = e->other;
		ev.taker = e->id;
		ev.qty = e->qty;
		(void)sv_observe(&c->sv, &ev);
		keep_trade(c, e);
	}
	/* An order that did not rest can appear in no later event. */
	rest = book_find(c->b, o->id);
	if (rest == NULL || rest->live != 1)
		sv_forget(&c->sv, o->id);
	if (reason)
		c->refused++;
	return reason;
}

/* cancelObserved: the engine cancels (ownership enforced), and the
 * detectors see it. */
static int
cancel_observed(con_t *c, uint64_t id, uint32_t account)
{
	sv_event_t ev;
	uint32_t i;
	int done = 0;

	c->out.n = 0;
	book_cancel(c->b, id, account, &c->out);
	for (i = 0; i < c->out.n; i++)
		if (c->out.ev[i].kind == BOOK_EV_CANCELED && c->out.ev[i].id == id)
			done = 1;
	if (!done)
		return BOOK_R_UNKNOWN_ID;
	memset(&ev, 0, sizeof ev);
	ev.kind = SV_CANCELLED;
	ev.user = account;
	ev.seq = ++c->seq;
	ev.order = id;
	(void)sv_observe(&c->sv, &ev);
	return 0;
}

/* The mid in ticks as their midTicks computes it: a float half-tick. */
static int
mid_ticks(const sig_snap_t *s, double *m)
{
	if (s->nb == 0 || s->na == 0)
		return 0;
	*m = ((double)s->bp[0] + (double)s->ap[0]) / 2;
	return 1;
}

/*
 * sim.NoiseTrader.Act: maybe one order, from a random identity of the
 * population, a market order (when there is a book) or a limit 0..6
 * ticks off the reference.  The draws are in Act's order, and the market
 * fraction is drawn only when there is a book (Go's && short-circuits).
 */
static int
noise_act(con_t *c, int64_t ref, int has_book, book_order_t *o)
{
	int64_t off;

	if (gorand_float64(&c->rng) >= NT_ACT_PROB)
		return 0;
	memset(o, 0, sizeof *o);
	o->account = CON_FLOW + (uint32_t)gorand_int31n(&c->rng, NT_POPULATION);
	o->side = BOOK_BUY;
	if (gorand_float64(&c->rng) < 0.5)
		o->side = BOOK_SELL;
	o->qty = NT_MIN_SIZE + gorand_int31n(&c->rng,
	    NT_MAX_SIZE - NT_MIN_SIZE + 1);
	if (has_book && gorand_float64(&c->rng) < NT_MARKET_FRAC) {
		o->type = BOOK_MARKET;
		o->tif = BOOK_IOC;
		return 1;
	}
	off = gorand_int31n(&c->rng, NT_MAX_OFFSET + 1);
	o->type = BOOK_LIMIT;
	o->tif = BOOK_GTC;
	o->price = o->side == BOOK_BUY ? ref - off : ref + off;
	if (o->price <= 0)
		o->price = 1;
	return 1;
}

/* stepOnce: pending spoofs age out, the agent acts on the current view,
 * and the signals absorb what happened. */
static void
step_once(con_t *c)
{
	sig_snap_t pre, post;
	book_order_t o;
	int64_t ref = CON_START, flow = 0;
	double pm, cm;
	uint32_t i;

	if (c->n_spoof && c->step - c->spoof_at >= SPOOF_STEPS) {
		for (i = 0; i < c->n_spoof; i++)
			(void)cancel_observed(c, c->spoof_ids[i], CON_SPOOFER);
		c->n_spoof = 0;
	}
	con_snapshot(c, &pre);
	if (mid_ticks(&pre, &pm))
		ref = (int64_t)pm;
	else if (c->b->last_price != BOOK_NO_PRICE && c->b->last_price > 0)
		ref = c->b->last_price;
	if (noise_act(c, ref, pre.nb > 0 || pre.na > 0, &o))
		(void)process(c, &o, &flow);
	con_snapshot(c, &post);
	(void)sig_ofi_observe(&c->ofi, &post);
	if (mid_ticks(&pre, &pm) && mid_ticks(&post, &cm))
		sig_lambda_add(&c->lam, (double)flow, cm - pm);
	c->cvd += flow;
	c->step++;
}

uint32_t
con_step(con_t *c, int n)
{
	int i;

	if (n < 1)
		n = 1;
	if (n > 200)
		n = 200;	/* one call must never wedge the frame */
	c->n_trades = 0;
	c->trades_more = 0;
	for (i = 0; i < n; i++)
		step_once(c);
	return c->n_trades;
}

/* submit: their Instrument.NewOrder refuses a non-positive quantity, or a
 * limit price that is not positive, before the engine sees it. */
int
con_submit(con_t *c, uint32_t account, int side, int market, int64_t price,
    int64_t qty, uint64_t *id)
{
	book_order_t o;
	int64_t flow = 0;
	int r;

	c->n_trades = 0;
	c->trades_more = 0;
	*id = 0;
	if (qty <= 0)
		return BOOK_R_BAD_QTY;
	if (!market && price <= 0)
		return BOOK_R_BAD_PRICE;
	memset(&o, 0, sizeof o);
	o.account = account;
	o.side = (uint8_t)(side ? BOOK_SELL : BOOK_BUY);
	o.type = (uint8_t)(market ? BOOK_MARKET : BOOK_LIMIT);
	o.tif = (uint8_t)(market ? BOOK_IOC : BOOK_GTC);
	o.price = market ? 0 : price;
	o.qty = qty;
	r = process(c, &o, &flow);
	c->cvd += flow;
	*id = o.id;
	return r;
}

int
con_cancel(con_t *c, uint64_t id, uint32_t account)
{
	return cancel_observed(c, id, account);
}

/* spoof: layered, far-from-touch size under a throwaway account, pulled
 * SPOOF_STEPS steps later by step_once.  Nothing here talks to the
 * detector; it only trades. */
int
con_spoof(con_t *c)
{
	sig_snap_t s;
	book_order_t o;
	int64_t flow = 0;
	uint32_t i;

	if (c->n_spoof)
		return -1;
	con_snapshot(c, &s);
	if (s.nb == 0)
		return -2;
	c->n_trades = 0;
	for (i = 0; i < SPOOF_LAYERS; i++) {
		memset(&o, 0, sizeof o);
		o.account = CON_SPOOFER;
		o.side = BOOK_BUY;
		o.type = BOOK_LIMIT;
		o.tif = BOOK_GTC;
		o.price = s.bp[0] - (int64_t)(2 + i);
		o.qty = SPOOF_SIZE;
		if (process(c, &o, &flow) == 0)
			c->spoof_ids[c->n_spoof++] = o.id;
	}
	c->cvd += flow;
	if (c->n_spoof == 0)
		return -3;
	c->spoof_at = c->step;
	return 0;
}

/* flood: a burst of tiny IOC buys priced where they can never trade --
 * quote stuffing, which an order-to-trade ratio scores. */
int
con_flood(con_t *c)
{
	sig_snap_t s;
	book_order_t o;
	int64_t flow = 0;
	uint32_t i;

	con_snapshot(c, &s);
	if (s.nb == 0)
		return -2;
	c->n_trades = 0;
	for (i = 0; i < FLOOD_ORDERS; i++) {
		memset(&o, 0, sizeof o);
		o.account = CON_FLOODER;
		o.side = BOOK_BUY;
		o.type = BOOK_LIMIT;
		o.tif = BOOK_IOC;
		o.price = s.bp[0] - (int64_t)(10 + i % 5);
		o.qty = 100;
		(void)process(c, &o, &flow);
	}
	c->cvd += flow;
	return 0;
}

/* signalsOut: every panel's current value. */
con_signals_t
con_signals(const con_t *c)
{
	con_signals_t r;
	sig_snap_t s;

	memset(&r, 0, sizeof r);
	con_snapshot(c, &s);
	r.step = c->step;
	if (s.nb && s.na) {
		r.has_mid = 1;
		r.mid = (s.bp[0] + s.ap[0]) / 2;	/* engine.MidPrice */
		r.spread = s.ap[0] - s.bp[0];
	}
	if (c->b->last_price != BOOK_NO_PRICE && c->b->last_price > 0)
		r.last = c->b->last_price;
	r.ofi = c->ofi.cum;
	r.cvd = c->cvd;
	r.imbalance = sig_depth_imbalance(&s, 5);
	r.best_imbalance = sig_depth_imbalance(&s, 1);
	if (c->lam.n >= CON_LAMBDA_MIN)
		r.lambda = sig_lambda_fit(&c->lam);
	return r;
}

struct open_walk {
	uint32_t	account, n, cap;
	uint64_t	*id;
	int64_t		*side, *price, *qty;
};

static void
open_one(const book_slot_t *o, void *arg)
{
	struct open_walk *w = arg;

	if (o->account != w->account || w->n == w->cap)
		return;
	w->id[w->n] = o->id;
	w->side[w->n] = o->side;
	w->price[w->n] = o->price;
	w->qty[w->n] = o->leaves;
	w->n++;
}

uint32_t
con_open_orders(const con_t *c, uint32_t account, uint64_t *id,
    int64_t *side, int64_t *price, int64_t *qty, uint32_t cap)
{
	struct open_walk w;

	w.account = account;
	w.n = 0;
	w.cap = cap;
	w.id = id;
	w.side = side;
	w.price = price;
	w.qty = qty;
	book_foreach_resting(c->b, open_one, &w);
	return w.n;
}
