/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/test_book.c -- the engine's unit tests, a differential test
 * against the reference matcher, and the determinism digest.
 *
 *	Pure C, no libxtc: the engine is a library-free core.
 *	Exit 0 = every case passed; each failing case prints its name.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "book.h"
#include "refbook.h"

static int failures;

#define CHECK(c) do { \
	if (!(c)) { \
		printf("  FAIL %s:%d: %s\n", __func__, __LINE__, #c); \
		failures++; \
		return; \
	} \
} while (0)

static book_event_t evbuf[8192];
static book_out_t out;

static void
reset_out(void)
{
	out.ev = evbuf;
	out.cap = sizeof evbuf / sizeof evbuf[0];
	out.n = 0;
	out.overflow = 0;
}

static book_t *
mkbook(uint32_t max_orders, int pro_rata, int64_t band_bp)
{
	static unsigned char *mem;
	static size_t memlen;
	book_config_t cfg;
	book_t *b;
	size_t n;

	memset(&cfg, 0, sizeof cfg);
	cfg.max_orders = max_orders;
	cfg.tick = 1;
	cfg.ref_price = 10000;		/* $100.00 in cents */
	cfg.band_pct_bp = band_bp;
	cfg.pro_rata = pro_rata;
	n = book_bytes(&cfg);
	if (n > memlen) {
		free(mem);
		if (posix_memalign((void **)&mem, 64, n) != 0)
			abort();
		memlen = n;
	}
	if (book_init(&b, mem, memlen, &cfg) != 0)
		abort();
	reset_out();
	return b;
}

static book_order_t
lim(uint64_t id, int side, int64_t px, int64_t qty)
{
	book_order_t o;
	memset(&o, 0, sizeof o);
	o.id = id;
	o.side = (uint8_t)side;
	o.type = BOOK_LIMIT;
	o.tif = BOOK_GTC;
	o.price = px;
	o.qty = qty;
	return o;
}

static int
count(uint8_t kind)
{
	uint32_t i;
	int n = 0;
	for (i = 0; i < out.n; i++)
		if (out.ev[i].kind == kind)
			n++;
	return n;
}

static const book_event_t *
nth(uint8_t kind, int k)
{
	uint32_t i;
	for (i = 0; i < out.n; i++)
		if (out.ev[i].kind == kind && k-- == 0)
			return &out.ev[i];
	return NULL;
}

/* ---- unit cases ---------------------------------------------------- */

static void
t_rest_and_bbo(void)
{
	book_t *b = mkbook(64, 0, 0);
	book_order_t o = lim(1, BOOK_BUY, 9990, 100);
	book_submit(b, &o, 0, &out);
	o = lim(2, BOOK_SELL, 10010, 50);
	book_submit(b, &o, 0, &out);
	CHECK(book_best(b, BOOK_BUY) == 9990);
	CHECK(book_best(b, BOOK_SELL) == 10010);
	CHECK(book_level_qty(b, BOOK_BUY, 9990) == 100);
	CHECK(count(BOOK_EV_ACCEPTED) == 2);
	CHECK(book_check(b) == 0);
}

/* The intrepidkarthi quickstart: alice 5 @101, bob 2 @101 behind her,
 * carol buys 4 @99; dave buys 6 @101 fills alice 5 then bob 1; erin's
 * post-only buy @101 is rejected. */
static void
t_quickstart(void)
{
	book_t *b = mkbook(64, 0, 0);
	book_order_t o;
	const book_event_t *t;

	o = lim(1, BOOK_SELL, 10100, 5);
	book_submit(b, &o, 0, &out);
	o = lim(2, BOOK_SELL, 10100, 2);
	book_submit(b, &o, 0, &out);
	o = lim(3, BOOK_BUY, 9900, 4);
	book_submit(b, &o, 0, &out);
	reset_out();
	o = lim(4, BOOK_BUY, 10100, 6);
	book_submit(b, &o, 0, &out);
	CHECK(count(BOOK_EV_TRADE) == 2);
	t = nth(BOOK_EV_TRADE, 0);
	CHECK(t->other == 1 && t->qty == 5 && t->price == 10100);
	t = nth(BOOK_EV_TRADE, 1);
	CHECK(t->other == 2 && t->qty == 1);
	CHECK(book_level_qty(b, BOOK_SELL, 10100) == 1);
	reset_out();
	o = lim(5, BOOK_BUY, 10100, 1);
	o.flags = BOOK_F_POST_ONLY;
	book_submit(b, &o, 0, &out);
	CHECK(count(BOOK_EV_REJECTED) == 1);
	CHECK(nth(BOOK_EV_REJECTED, 0)->reason == BOOK_R_POST_ONLY_CROSS);
	CHECK(book_check(b) == 0);
}

static void
t_market_sweeps_levels(void)
{
	book_t *b = mkbook(64, 0, 0);
	book_order_t o;
	o = lim(1, BOOK_SELL, 10001, 10);
	book_submit(b, &o, 0, &out);
	o = lim(2, BOOK_SELL, 10002, 10);
	book_submit(b, &o, 0, &out);
	o = lim(3, BOOK_SELL, 10005, 10);
	book_submit(b, &o, 0, &out);
	reset_out();
	memset(&o, 0, sizeof o);
	o.id = 9;
	o.side = BOOK_BUY;
	o.type = BOOK_MARKET;
	o.qty = 25;
	book_submit(b, &o, 0, &out);
	CHECK(count(BOOK_EV_TRADE) == 3);
	CHECK(nth(BOOK_EV_TRADE, 2)->price == 10005);
	CHECK(nth(BOOK_EV_TRADE, 2)->qty == 5);
	CHECK(book_best(b, BOOK_SELL) == 10005);
	CHECK(book_level_qty(b, BOOK_SELL, 10005) == 5);
	/* A market order that outsizes the book cancels its remainder. */
	reset_out();
	o.id = 10;
	o.qty = 100;
	book_submit(b, &o, 0, &out);
	CHECK(count(BOOK_EV_TRADE) == 1);
	CHECK(nth(BOOK_EV_CANCELED, 0)->qty == 95);
	CHECK(book_best(b, BOOK_SELL) == BOOK_NO_PRICE);
	CHECK(book_check(b) == 0);
}

static void
t_ioc_fok(void)
{
	book_t *b = mkbook(64, 0, 0);
	book_order_t o;
	o = lim(1, BOOK_SELL, 10000, 10);
	book_submit(b, &o, 0, &out);
	reset_out();
	o = lim(2, BOOK_BUY, 10000, 15);
	o.tif = BOOK_FOK;
	book_submit(b, &o, 0, &out);
	CHECK(count(BOOK_EV_TRADE) == 0);
	CHECK(nth(BOOK_EV_REJECTED, 0)->reason == BOOK_R_FOK_UNFILLABLE);
	CHECK(book_level_qty(b, BOOK_SELL, 10000) == 10);
	reset_out();
	o = lim(3, BOOK_BUY, 10000, 15);
	o.tif = BOOK_IOC;
	book_submit(b, &o, 0, &out);
	CHECK(count(BOOK_EV_TRADE) == 1);
	CHECK(nth(BOOK_EV_CANCELED, 0)->qty == 5);
	CHECK(nth(BOOK_EV_CANCELED, 0)->reason == BOOK_R_IOC);
	CHECK(book_best(b, BOOK_BUY) == BOOK_NO_PRICE);
	CHECK(book_check(b) == 0);
}

static void
t_cancel_reduce_replace(void)
{
	book_t *b = mkbook(64, 0, 0);
	book_order_t o;
	o = lim(1, BOOK_BUY, 9990, 10);
	o.account = 7;
	book_submit(b, &o, 0, &out);
	o = lim(2, BOOK_BUY, 9990, 10);
	o.account = 8;
	book_submit(b, &o, 0, &out);
	/* Reduce keeps priority: order 1 is still first at 9990. */
	reset_out();
	book_reduce(b, 1, 7, 4, &out);
	CHECK(count(BOOK_EV_REDUCED) == 1);
	CHECK(book_level_qty(b, BOOK_BUY, 9990) == 14);
	o = lim(3, BOOK_SELL, 9990, 4);
	reset_out();
	book_submit(b, &o, 0, &out);
	CHECK(nth(BOOK_EV_TRADE, 0)->other == 1);
	/* Another account cannot cancel order 2. */
	reset_out();
	book_cancel(b, 2, 7, &out);
	CHECK(nth(BOOK_EV_REJECTED, 0)->reason == BOOK_R_UNKNOWN_ID);
	/* Replace loses priority and moves price. */
	reset_out();
	book_replace(b, 2, 8, 20, 9995, 6, 0, &out);
	CHECK(count(BOOK_EV_REPLACED) == 1);
	CHECK(book_level_qty(b, BOOK_BUY, 9990) == 0);
	CHECK(book_level_qty(b, BOOK_BUY, 9995) == 6);
	CHECK(book_find(b, 2) == NULL && book_find(b, 20) != NULL);
	reset_out();
	book_cancel(b, 20, 8, &out);
	CHECK(count(BOOK_EV_CANCELED) == 1);
	CHECK(book_best(b, BOOK_BUY) == BOOK_NO_PRICE);
	CHECK(book_check(b) == 0);
}

static void
t_cancel_all(void)
{
	book_t *b = mkbook(64, 0, 0);
	book_order_t o;
	int i;
	for (i = 0; i < 10; i++) {
		o = lim((uint64_t)(i + 1), i & 1, i & 1 ? 10010 + i : 9990 - i, 5);
		o.account = (uint32_t)(i % 3 == 0 ? 42 : 43);
		book_submit(b, &o, 0, &out);
	}
	reset_out();
	CHECK(book_cancel_all(b, 42, &out) == 4);
	CHECK(count(BOOK_EV_CANCELED) == 4);
	CHECK(book_check(b) == 0);
}

static void
t_iceberg(void)
{
	book_t *b = mkbook(64, 0, 0);
	book_order_t o = lim(1, BOOK_SELL, 10000, 100);
	o.display = 10;
	book_submit(b, &o, 0, &out);
	o = lim(2, BOOK_SELL, 10000, 5);
	book_submit(b, &o, 0, &out);
	CHECK(book_level_qty(b, BOOK_SELL, 10000) == 15);	/* 10 shown + 5 */
	reset_out();
	/* Buy 12: 10 from the iceberg's peak, then it refills to the back,
	 * so the next 2 come from order 2. */
	o = lim(3, BOOK_BUY, 10000, 12);
	book_submit(b, &o, 0, &out);
	CHECK(count(BOOK_EV_TRADE) == 2);
	CHECK(nth(BOOK_EV_TRADE, 0)->other == 1 && nth(BOOK_EV_TRADE, 0)->qty == 10);
	CHECK(nth(BOOK_EV_TRADE, 1)->other == 2 && nth(BOOK_EV_TRADE, 1)->qty == 2);
	CHECK(book_find(b, 1)->leaves == 90 && book_find(b, 1)->shown == 10);
	CHECK(book_level_qty(b, BOOK_SELL, 10000) == 13);
	CHECK(book_check(b) == 0);
}

static void
t_stp(void)
{
	static const struct {
		int mode;
		int trades, incoming_left, resting_left;
	} want[] = {
		{ BOOK_STP_CANCEL_NEWEST, 0, 0, 10 },
		{ BOOK_STP_CANCEL_OLDEST, 1, 0, -1 },
		{ BOOK_STP_CANCEL_BOTH, 0, 0, -1 },
		{ BOOK_STP_DECREMENT, 0, 0, 4 },
		{ BOOK_STP_ALLOW, 1, 0, 4 },
	};
	size_t k;
	for (k = 0; k < sizeof want / sizeof want[0]; k++) {
		book_t *b = mkbook(64, 0, 0);
		book_order_t o;
		const book_slot_t *r;
		o = lim(1, BOOK_SELL, 10000, 10);
		o.account = 5;
		book_submit(b, &o, 0, &out);
		o = lim(2, BOOK_SELL, 10001, 10);	/* another account, deeper */
		o.account = 6;
		book_submit(b, &o, 0, &out);
		reset_out();
		o = lim(3, BOOK_BUY, 10001, 6);
		o.account = 5;
		o.stp = (uint8_t)want[k].mode;
		book_submit(b, &o, 0, &out);
		r = book_find(b, 1);
		if (want[k].resting_left < 0)
			CHECK(r == NULL);
		else
			CHECK(r != NULL && r->leaves == want[k].resting_left);
		CHECK(count(BOOK_EV_TRADE) == want[k].trades ||
		    (want[k].mode == BOOK_STP_CANCEL_OLDEST &&
		    count(BOOK_EV_TRADE) == 1));
		CHECK(book_find(b, 3) == NULL);
		CHECK(book_check(b) == 0);
	}
}

static void
t_stops(void)
{
	book_t *b = mkbook(64, 0, 0);
	book_order_t o;
	o = lim(1, BOOK_SELL, 10000, 10);
	book_submit(b, &o, 0, &out);
	o = lim(2, BOOK_SELL, 10050, 10);
	book_submit(b, &o, 0, &out);
	/* A buy stop at 10000: triggers when a trade prints at >= 10000. */
	memset(&o, 0, sizeof o);
	o.id = 3;
	o.side = BOOK_BUY;
	o.type = BOOK_STOP;
	o.stop = 10000;
	o.qty = 15;
	book_submit(b, &o, 0, &out);
	CHECK(count(BOOK_EV_TRADE) == 0);
	reset_out();
	o = lim(4, BOOK_BUY, 10000, 1);
	book_submit(b, &o, 0, &out);
	/* 1 @10000 prints, the stop fires as a market buy 15: 9 @10000 then
	 * 6 @10050. */
	CHECK(count(BOOK_EV_TRIGGERED) == 1);
	CHECK(count(BOOK_EV_TRADE) == 3);
	CHECK(nth(BOOK_EV_TRADE, 2)->price == 10050);
	CHECK(book_level_qty(b, BOOK_SELL, 10050) == 4);
	CHECK(book_check(b) == 0);
}

static void
t_stop_limit_and_oco(void)
{
	book_t *b = mkbook(64, 0, 0);
	book_order_t o;
	/* OCO bracket on a long: take-profit sell @10100, stop-limit sell
	 * triggered at 9900 limit 9890.  One printing cancels the other. */
	o = lim(10, BOOK_SELL, 10100, 5);
	o.oco = 11;
	book_submit(b, &o, 0, &out);
	memset(&o, 0, sizeof o);
	o.id = 11;
	o.side = BOOK_SELL;
	o.type = BOOK_STOP_LIMIT;
	o.stop = 9900;
	o.price = 9890;
	o.qty = 5;
	o.oco = 10;
	book_submit(b, &o, 0, &out);
	o = lim(1, BOOK_BUY, 9900, 3);
	book_submit(b, &o, 0, &out);
	reset_out();
	/* A sell prints at 9900: the stop-limit triggers, cancels the
	 * take-profit (OCO), and rests/trades at 9890. */
	o = lim(2, BOOK_SELL, 9900, 1);
	book_submit(b, &o, 0, &out);
	CHECK(count(BOOK_EV_TRIGGERED) == 1);
	CHECK(book_find(b, 10) == NULL);
	CHECK(nth(BOOK_EV_CANCELED, 0)->reason == BOOK_R_OCO);
	CHECK(book_level_qty(b, BOOK_SELL, 10100) == 0);
	CHECK(book_check(b) == 0);
}

static void
t_trailing_stop(void)
{
	book_t *b = mkbook(64, 0, 0);
	book_order_t o;
	int i;
	/* Sell trailing stop, distance 20, from a last trade of 10000. */
	o = lim(1, BOOK_BUY, 9000, 100);	/* liquidity below */
	book_submit(b, &o, 0, &out);
	o = lim(2, BOOK_SELL, 10000, 1);
	book_submit(b, &o, 0, &out);
	o = lim(3, BOOK_BUY, 10000, 1);
	book_submit(b, &o, 0, &out);		/* last = 10000 */
	memset(&o, 0, sizeof o);
	o.id = 4;
	o.side = BOOK_SELL;
	o.type = BOOK_TRAILING_STOP;
	o.stop = 20;
	o.qty = 10;
	book_submit(b, &o, 0, &out);
	CHECK(book_find(b, 4)->stop == 9980);
	/* Price walks up to 10050: the trigger follows to 10030. */
	for (i = 1; i <= 5; i++) {
		book_order_t s = lim(100 + (uint64_t)i, BOOK_SELL, 10000 + i * 10, 1);
		book_order_t t = lim(200 + (uint64_t)i, BOOK_BUY, 10000 + i * 10, 1);
		book_submit(b, &s, 0, &out);
		book_submit(b, &t, 0, &out);
	}
	CHECK(book_find(b, 4) != NULL && book_find(b, 4)->stop == 10030);
	/* A print at 10030 fires it into the 9000 bid. */
	reset_out();
	o = lim(300, BOOK_SELL, 10030, 1);
	book_submit(b, &o, 0, &out);
	o = lim(301, BOOK_BUY, 10030, 1);
	book_submit(b, &o, 0, &out);
	CHECK(count(BOOK_EV_TRIGGERED) == 1);
	CHECK(book_find(b, 4) == NULL);
	CHECK(book_level_qty(b, BOOK_BUY, 9000) == 90);
	CHECK(book_check(b) == 0);
}

static void
t_pegged(void)
{
	book_t *b = mkbook(64, 0, 0);
	book_order_t o;
	o = lim(1, BOOK_BUY, 9990, 10);
	book_submit(b, &o, 0, &out);
	o = lim(2, BOOK_SELL, 10010, 10);
	book_submit(b, &o, 0, &out);
	memset(&o, 0, sizeof o);
	o.id = 3;
	o.side = BOOK_BUY;
	o.type = BOOK_PEGGED;
	o.peg = BOOK_PEG_MID;
	o.qty = 5;
	book_submit(b, &o, 0, &out);
	CHECK(book_find(b, 3) != NULL && book_find(b, 3)->price == 10000);
	o.id = 4;
	o.peg = BOOK_PEG_BID;
	o.peg_offset = 1;
	book_submit(b, &o, 0, &out);
	CHECK(book_find(b, 4)->price == 10001);
	CHECK(book_check(b) == 0);
}

static void
t_gtd_day_expiry(void)
{
	book_t *b = mkbook(64, 0, 0);
	book_order_t o = lim(1, BOOK_BUY, 9990, 10);
	o.tif = BOOK_GTD;
	o.expire_ns = 1000;
	book_submit(b, &o, 0, &out);
	o = lim(2, BOOK_BUY, 9991, 10);
	o.tif = BOOK_DAY;
	book_submit(b, &o, 0, &out);
	o = lim(3, BOOK_BUY, 9992, 10);
	book_submit(b, &o, 0, &out);
	reset_out();
	CHECK(book_expire(b, 999, &out) == 0);
	CHECK(book_expire(b, 1000, &out) == 1);
	CHECK(book_find(b, 1) == NULL);
	book_set_phase(b, BOOK_CLOSED, &out);
	CHECK(book_find(b, 2) == NULL);
	CHECK(book_find(b, 3) != NULL);		/* GTC survives the close */
	CHECK(book_check(b) == 0);
}

static void
t_phases_and_auction(void)
{
	book_t *b = mkbook(256, 0, 0);
	book_order_t o;
	const book_event_t *t;
	uint32_t i;
	book_set_phase(b, BOOK_PREOPEN, &out);
	/* A crossed pre-open book: bids 10 @100.10, 20 @100.05; asks
	 * 15 @99.95, 10 @100.00, 10 @100.10.  25 shares can cross at 100.00
	 * and at 100.05 (bids >= p = 30, asks <= p = 25), both leaving an
	 * imbalance of 5, so the tie goes to the price nearest the
	 * reference, 100.00 (the NASDAQ cross rule: maximum volume, then
	 * minimum imbalance, then nearest the reference). */
	o = lim(1, BOOK_BUY, 10010, 10);
	book_submit(b, &o, 0, &out);
	o = lim(2, BOOK_BUY, 10005, 20);
	book_submit(b, &o, 0, &out);
	o = lim(3, BOOK_SELL, 9995, 15);
	book_submit(b, &o, 0, &out);
	o = lim(4, BOOK_SELL, 10000, 10);
	book_submit(b, &o, 0, &out);
	o = lim(5, BOOK_SELL, 10010, 10);
	book_submit(b, &o, 0, &out);
	CHECK(count(BOOK_EV_TRADE) == 0);	/* pre-open does not match */
	reset_out();
	book_set_phase(b, BOOK_CONTINUOUS, &out);
	CHECK(count(BOOK_EV_TRADE) > 0);
	for (i = 0; (t = nth(BOOK_EV_TRADE, (int)i)) != NULL; i++)
		CHECK(t->price == 10000);	/* one uniform price */
	CHECK(book_best(b, BOOK_BUY) < book_best(b, BOOK_SELL));
	CHECK(book_level_qty(b, BOOK_BUY, 10005) == 5);
	/* Halted: new orders refused, cancels allowed. */
	book_set_phase(b, BOOK_HALTED, &out);
	reset_out();
	o = lim(9, BOOK_BUY, 9000, 1);
	book_submit(b, &o, 0, &out);
	CHECK(nth(BOOK_EV_REJECTED, 0)->reason == BOOK_R_PHASE);
	book_cancel(b, 5, 0, &out);
	CHECK(count(BOOK_EV_CANCELED) == 1);
	CHECK(book_check(b) == 0);
}

static void
t_circuit_breaker(void)
{
	book_t *b = mkbook(64, 0, 500);	/* +-5% of 100.00 */
	book_order_t o = lim(1, BOOK_BUY, 10400, 1);
	book_submit(b, &o, 0, &out);
	CHECK(count(BOOK_EV_ACCEPTED) == 1);
	o = lim(2, BOOK_BUY, 10600, 1);
	book_submit(b, &o, 0, &out);
	CHECK(nth(BOOK_EV_REJECTED, 0)->reason == BOOK_R_BAND);
}

static void
t_pro_rata(void)
{
	book_t *b = mkbook(64, 1, 0);
	book_order_t o;
	o = lim(1, BOOK_SELL, 10000, 30);
	book_submit(b, &o, 0, &out);
	o = lim(2, BOOK_SELL, 10000, 10);
	book_submit(b, &o, 0, &out);
	reset_out();
	o = lim(3, BOOK_BUY, 10000, 20);
	book_submit(b, &o, 0, &out);
	/* 20 against 30:10 -> 15 and 5. */
	CHECK(book_find(b, 1)->leaves == 15);
	CHECK(book_find(b, 2)->leaves == 5);
	CHECK(book_check(b) == 0);
}

static void
t_rejects(void)
{
	book_t *b = mkbook(4, 0, 0);
	book_order_t o = lim(1, BOOK_BUY, 9990, 0);
	book_submit(b, &o, 0, &out);
	CHECK(nth(BOOK_EV_REJECTED, 0)->reason == BOOK_R_BAD_QTY);
	reset_out();
	o = lim(1, BOOK_BUY, 9990, 1);
	book_submit(b, &o, 0, &out);
	book_submit(b, &o, 0, &out);
	CHECK(nth(BOOK_EV_REJECTED, 0)->reason == BOOK_R_DUPLICATE_ID);
	reset_out();
	o = lim(2, BOOK_BUY, 1, 1);		/* far outside the band */
	book_submit(b, &o, 0, &out);
	CHECK(nth(BOOK_EV_REJECTED, 0)->reason == BOOK_R_BAD_PRICE);
	reset_out();
	o = lim(3, BOOK_BUY, 9991, 1);
	book_submit(b, &o, 0, &out);
	o = lim(4, BOOK_BUY, 9992, 1);
	book_submit(b, &o, 0, &out);
	o = lim(5, BOOK_BUY, 9993, 1);
	book_submit(b, &o, 0, &out);
	o = lim(6, BOOK_BUY, 9994, 1);		/* pool of 4 is full */
	book_submit(b, &o, 0, &out);
	CHECK(nth(BOOK_EV_REJECTED, 0)->reason == BOOK_R_FULL);
	CHECK(book_check(b) == 0);
}

/* ---- differential: the engine against the naive reference ---------- */

static uint64_t rng = 0x9e3779b97f4a7c15ull;

static uint64_t
rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

static void
t_differential(void)
{
	static ref_book_t ref;
	book_t *b = mkbook(4096, 0, 0);
	uint64_t next_id = 1, ids[4096];
	int nids = 0, step;
	int64_t eng_vol = 0;

	memset(&ref, 0, sizeof ref);
	for (step = 0; step < 200000; step++) {
		uint64_t r = rnd();
		int op = (int)(r % 100);
		reset_out();
		if (op < 55 || nids == 0) {
			/* limit, near the touch; 1 in 8 IOC, 1 in 16 FOK, 1 in
			 * 16 post-only, 1 in 20 market */
			int side = (int)((r >> 8) & 1);
			int64_t px = 10000 + (int64_t)((r >> 9) % 41) - 20;
			int64_t qty = 1 + (int64_t)((r >> 20) % 200);
			int tif = ((r >> 30) % 8) == 0 ? 1 :
			    ((r >> 33) % 16) == 0 ? 2 : 0;
			int post = tif == 0 && ((r >> 37) % 16) == 0;
			int market = ((r >> 41) % 20) == 0;
			book_order_t o = lim(next_id, side, px, qty);
			uint32_t i;
			int64_t got = 0;
			if (market) {
				o.type = BOOK_MARKET;
				o.price = 0;
				tif = tif == 2 ? 2 : 1;
				post = 0;	/* post-only is a limit-order flag */
			}
			o.tif = (uint8_t)(tif == 1 ? BOOK_IOC : tif == 2 ?
			    BOOK_FOK : BOOK_GTC);
			if (post)
				o.flags = BOOK_F_POST_ONLY;
			book_submit(b, &o, 0, &out);
			for (i = 0; i < out.n; i++)
				if (out.ev[i].kind == BOOK_EV_TRADE)
					got += out.ev[i].qty;
			/* A market order on an empty side is rejected by the
			 * engine and is a no-op in the reference too. */
			{
				int64_t want = ref_submit(&ref, next_id, side,
				    market, px, qty, tif, post);
				if (got != want) {
					printf("  step %d: id %" PRIu64 " side %d %s "
					    "px %" PRId64 " qty %" PRId64 " tif %d "
					    "post %d: engine %" PRId64 " ref %" PRId64
					    "\n", step, next_id, side,
					    market ? "MKT" : "LMT", px, qty, tif,
					    post, got, want);
					for (i = 0; i < out.n; i++)
						printf("    ev %u kind %u id %" PRIu64
						    " other %" PRIu64 " px %" PRId64
						    " q %" PRId64 " r %u\n", i,
						    out.ev[i].kind, out.ev[i].id,
						    out.ev[i].other, out.ev[i].price,
						    out.ev[i].qty, out.ev[i].reason);
				}
				CHECK(got == want);
			}
			eng_vol += got;
			if (book_find(b, next_id) != NULL && nids < 4096)
				ids[nids++] = next_id;
			next_id++;
		} else if (op < 85) {
			int k = (int)((r >> 8) % (uint64_t)nids);
			int rc_ref = ref_cancel(&ref, ids[k]);
			book_cancel(b, ids[k], 0, &out);
			CHECK((rc_ref == 0) == (count(BOOK_EV_CANCELED) == 1));
			ids[k] = ids[--nids];
		} else if (op < 95) {
			int k = (int)((r >> 8) % (uint64_t)nids);
			const book_slot_t *s = book_find(b, ids[k]);
			if (s != NULL && s->leaves > 1) {
				int64_t q = 1 + (int64_t)((r >> 20) %
				    (uint64_t)(s->leaves - 1));
				CHECK(ref_reduce(&ref, ids[k], q) == 0);
				book_reduce(b, ids[k], 0, q, &out);
				CHECK(count(BOOK_EV_REDUCED) == 1);
			}
		} else {
			/* replace = cancel + new limit (priority lost) */
			int k = (int)((r >> 8) % (uint64_t)nids);
			int s;
			int i = ref_find(&ref, ids[k], &s);
			int64_t px = 10000 + (int64_t)((r >> 9) % 41) - 20;
			int64_t qty = 1 + (int64_t)((r >> 20) % 200);
			uint32_t e;
			int64_t got = 0;
			if (i >= 0) {
				(void)ref_cancel(&ref, ids[k]);
				book_replace(b, ids[k], 0, next_id, px, qty, 0,
				    &out);
				for (e = 0; e < out.n; e++)
					if (out.ev[e].kind == BOOK_EV_TRADE)
						got += out.ev[e].qty;
				CHECK(got == ref_submit(&ref, next_id, s, 0, px,
				    qty, 0, 0));
				eng_vol += got;
				ids[k] = next_id;
				if (book_find(b, next_id) == NULL)
					ids[k] = ids[--nids];
				next_id++;
			} else
				ids[k] = ids[--nids];
		}
		CHECK(out.overflow == 0);
		if ((step & 1023) == 0) {
			int64_t p;
			int side;
			CHECK(book_check(b) == 0);
			for (side = 0; side < 2; side++)
				for (p = 9970; p <= 10030; p++)
					CHECK(book_level_qty(b, side, p) ==
					    ref_level(&ref, side, p));
		}
	}
	CHECK(eng_vol == ref.volume);
	CHECK(book_check(b) == 0);
	printf("  differential: 200000 commands, %" PRIu64 " trades, "
	    "volume %" PRId64 ", engine == reference\n", b->trades, eng_vol);
}

/* ---- determinism: same commands, same events, same digest --------- */

static uint64_t
run_tape(uint64_t seed, uint64_t *ev_hash)
{
	book_t *b = mkbook(4096, 0, 0);
	uint64_t h = 0xcbf29ce484222325ull, id;
	int i;
	rng = seed;
	for (id = 1; id <= 50000; id++) {
		uint64_t r = rnd();
		book_order_t o = lim(id, (int)(r & 1),
		    10000 + (int64_t)((r >> 1) % 31) - 15,
		    1 + (int64_t)((r >> 8) % 100));
		uint32_t e;
		reset_out();
		if ((r >> 20) % 4 == 0 && id > 10)
			book_cancel(b, id - 1 - (r >> 24) % 10, 0, &out);
		else
			book_submit(b, &o, (int64_t)id, &out);
		for (e = 0; e < out.n; e++) {
			const unsigned char *p = (const void *)&out.ev[e];
			for (i = 0; i < (int)sizeof out.ev[e]; i++) {
				h ^= p[i];
				h *= 0x100000001b3ull;
			}
		}
	}
	*ev_hash = h;
	return book_digest(b);
}

/*
 * A book that stays small but sees many orders over its life (every
 * venue, and the browser console) must keep accepting them: the id map
 * may not fill up with the ids of orders long gone.  64 slots give a
 * 128-entry map with 8 orders live; before the fix (tombstones that
 * never came back) the first order refused BOOK_R_FULL was number
 * 11,142.  50,000 is 4.5x that.
 */
static void
t_id_churn(void)
{
	book_t *b = mkbook(64, 0, 0);
	book_order_t o;
	uint64_t id;

	for (id = 1; id <= 50000; id++) {
		reset_out();
		o = lim(id, (int)(id & 1), id & 1 ? 10010 : 9990, 10);
		book_submit(b, &o, 0, &out);
		if (count(BOOK_EV_REJECTED) != 0)
			printf("  id_churn: order %" PRIu64 " refused\n", id);
		CHECK(count(BOOK_EV_REJECTED) == 0);
		if (id > 8) {
			reset_out();
			book_cancel(b, id - 8, 0, &out);
			CHECK(count(BOOK_EV_CANCELED) == 1);
		}
	}
	for (id = 49993; id <= 50000; id++)
		CHECK(book_find(b, id) != NULL && book_find(b, id)->live == 1);
	CHECK(book_find(b, 49992) == NULL);
	CHECK(b->map_used == 8);
	CHECK(book_check(b) == 0);
}

/* Random victims, up to 64 live (half the map): deletes land in the
 * middle of probe runs, including runs that wrap the table. */
static void
t_id_churn_random(void)
{
	book_t *b = mkbook(64, 0, 0);
	book_order_t o;
	uint64_t live[64], r = 7, id;
	int n = 0, i;

	for (id = 1; id <= 200000; id++) {
		r ^= r << 13;
		r ^= r >> 7;
		r ^= r << 17;
		if (n == 64 || (n > 0 && (r & 3) == 0)) {
			int k = (int)((r >> 8) % (uint64_t)n);
			reset_out();
			book_cancel(b, live[k], 0, &out);
			CHECK(count(BOOK_EV_CANCELED) == 1);
			CHECK(book_find(b, live[k]) == NULL);
			live[k] = live[--n];
		}
		reset_out();
		o = lim(id, (int)(r >> 20 & 1), r >> 20 & 1 ? 10010 : 9990, 10);
		book_submit(b, &o, 0, &out);
		CHECK(count(BOOK_EV_REJECTED) == 0);
		live[n++] = id;
	}
	for (i = 0; i < n; i++)
		CHECK(book_find(b, live[i]) != NULL);
	CHECK(b->map_used == (uint32_t)n);
	CHECK(book_check(b) == 0);
}

static void
t_determinism(void)
{
	uint64_t e1, e2, d1, d2, d3, e3;
	d1 = run_tape(42, &e1);
	d2 = run_tape(42, &e2);
	d3 = run_tape(43, &e3);
	CHECK(d1 == d2 && e1 == e2);
	CHECK(d1 != d3);
	printf("  determinism: seed 42 digest %016" PRIx64 " events %016" PRIx64
	    " (twice), seed 43 differs\n", d1, e1);
}

int
main(int argc, char **argv)
{
	static const struct {
		const char *name;
		void (*fn)(void);
	} cases[] = {
		{ "rest_and_bbo", t_rest_and_bbo },
		{ "quickstart", t_quickstart },
		{ "market_sweeps_levels", t_market_sweeps_levels },
		{ "ioc_fok", t_ioc_fok },
		{ "cancel_reduce_replace", t_cancel_reduce_replace },
		{ "cancel_all", t_cancel_all },
		{ "iceberg", t_iceberg },
		{ "stp", t_stp },
		{ "stops", t_stops },
		{ "stop_limit_and_oco", t_stop_limit_and_oco },
		{ "trailing_stop", t_trailing_stop },
		{ "pegged", t_pegged },
		{ "gtd_day_expiry", t_gtd_day_expiry },
		{ "phases_and_auction", t_phases_and_auction },
		{ "circuit_breaker", t_circuit_breaker },
		{ "pro_rata", t_pro_rata },
		{ "rejects", t_rejects },
		{ "id_churn", t_id_churn },
		{ "id_churn_random", t_id_churn_random },
		{ "differential", t_differential },
		{ "determinism", t_determinism },
	};
	size_t i;
	(void)argv;
	if (argc > 1) {
		printf("usage: test_book\n\nThe l2book engine's unit, "
		    "differential and determinism tests.\n");
		return 0;
	}
	for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		int before = failures;
		cases[i].fn();
		printf("%s %s\n", failures == before ? "ok  " : "FAIL",
		    cases[i].name);
	}
	if (failures) {
		printf("test_book: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_book: all %zu cases passed\n", i);
	return 0;
}
