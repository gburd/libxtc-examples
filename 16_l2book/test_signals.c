/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/test_signals.c -- known-answer tests for the console's
 * signals, surveillance, Go-compatible RNG and market.
 *
 *	The signal and detector cases are intrepidkarthi/orderbook's own
 *	(the pkg/signals tests, pkg/surveillance/surveillance_test.go at
 *	3fffc25), with their inputs and expected values.  The RNG cases are
 *	Go's (math/rand regress_test.go, seed 0) and `go run` output.  The
 *	market cases are their console's (cmd/obwasm, built to wasm and run
 *	headless): same seed, same steps, same book and signals.
 *	Exit 0 = every case passed.
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"

static int failures;

#define CHECK(c) do { \
	if (!(c)) { \
		printf("  FAIL %s:%d: %s\n", __func__, __LINE__, #c); \
		failures++; \
		return; \
	} \
} while (0)

static int
approx(double a, double b)
{
	return fabs(a - b) < 1e-9;
}

/* snap({{price, qty}...}, {{price, qty}...}), best first. */
static sig_snap_t
snap(const int64_t (*bids)[2], uint32_t nb, const int64_t (*asks)[2],
    uint32_t na)
{
	sig_snap_t s;
	uint32_t i;

	memset(&s, 0, sizeof s);
	s.nb = nb;
	s.na = na;
	for (i = 0; i < nb; i++) {
		s.bp[i] = bids[i][0];
		s.bq[i] = bids[i][1];
	}
	for (i = 0; i < na; i++) {
		s.ap[i] = asks[i][0];
		s.aq[i] = asks[i][1];
	}
	return s;
}

/* ofi_test.go */
static void
t_ofi(void)
{
	static const int64_t b5[][2] = { { 100, 5 } }, b8[][2] = { { 100, 8 } };
	static const int64_t a5[][2] = { { 101, 5 } }, a9[][2] = { { 101, 9 } };
	static const int64_t b101_3[][2] = { { 101, 3 } };
	static const int64_t a102_5[][2] = { { 102, 5 } };
	static const int64_t a102_4[][2] = { { 102, 4 } };
	static const int64_t a101_2[][2] = { { 101, 2 } };
	sig_snap_t p, c, s0, s1, s2;
	sig_ofi_t o;

	p = snap(b5, 1, a5, 1);
	CHECK(sig_ofi_step(&p, &p) == 0);			/* NoChange */
	c = snap(b8, 1, a5, 1);
	CHECK(sig_ofi_step(&p, &c) == 3);			/* BidQtyUp */
	c = snap(b5, 1, a9, 1);
	CHECK(sig_ofi_step(&p, &c) == -4);			/* AskQtyUp */
	p = snap(b5, 1, a102_5, 1);
	c = snap(b101_3, 1, a102_5, 1);
	CHECK(sig_ofi_step(&p, &c) == 3);			/* BidPriceUp */
	p = snap(b5, 1, a102_4, 1);
	c = snap(b5, 1, a101_2, 1);
	CHECK(sig_ofi_step(&p, &c) == -2);			/* AskPriceDown */
	/* Accumulator: prime, +3, -4, cumulative -1. */
	memset(&o, 0, sizeof o);
	s0 = snap(b5, 1, a5, 1);
	s1 = snap(b8, 1, a5, 1);
	s2 = snap(b8, 1, a9, 1);
	CHECK(sig_ofi_observe(&o, &s0) == 0);
	CHECK(sig_ofi_observe(&o, &s1) == 3);
	CHECK(sig_ofi_observe(&o, &s2) == -4);
	CHECK(o.cum == -1);
	/* Not in their tests, from the definition: the ask stepping up
	 * pulls Q (e = +Q = +4); the bid stepping down pulls Q (e = -5). */
	p = snap(b5, 1, a102_4, 1);
	c = snap(b5, 1, a102_5, 1);
	c.ap[0] = 103;
	CHECK(sig_ofi_step(&p, &c) == 4);
	c = snap(b5, 1, a102_4, 1);
	c.bp[0] = 99;
	CHECK(sig_ofi_step(&p, &c) == -5);
	/* A side missing from either snapshot contributes 0. */
	c = snap(b8, 1, a5, 0);
	CHECK(sig_ofi_step(&s0, &c) == 3);
}

/* imbalance_test.go */
static void
t_imbalance(void)
{
	static const int64_t b5[][2] = { { 100, 5 } }, a5[][2] = { { 101, 5 } };
	static const int64_t b8[][2] = { { 100, 8 } }, a2[][2] = { { 101, 2 } };
	static const int64_t b2[][2] = { { 100, 2 } }, a8[][2] = { { 101, 8 } };
	static const int64_t bd[][2] = { { 100, 1 }, { 99, 9 } };
	static const int64_t ad[][2] = { { 101, 1 }, { 102, 1 } };
	sig_snap_t s;

	s = snap(b5, 1, a5, 1);
	CHECK(approx(sig_depth_imbalance(&s, 1), 0));
	s = snap(b8, 1, a2, 1);
	CHECK(approx(sig_depth_imbalance(&s, 1), 0.6));
	s = snap(b2, 1, a8, 1);
	CHECK(approx(sig_depth_imbalance(&s, 1), -0.6));
	s = snap(bd, 2, ad, 2);
	CHECK(approx(sig_depth_imbalance(&s, 1), 0));
	CHECK(approx(sig_depth_imbalance(&s, 2), 8.0 / 12.0));
	s = snap(b5, 0, a5, 0);
	CHECK(sig_depth_imbalance(&s, 1) == 0);
	s = snap(b5, 1, a5, 0);
	CHECK(approx(sig_depth_imbalance(&s, 1), 1));
	s = snap(b5, 0, a5, 1);
	CHECK(approx(sig_depth_imbalance(&s, 1), -1));
	CHECK(sig_depth_imbalance(&s, 0) == 0);
}

/* regression_test.go and kyle_test.go */
static void
t_linreg_lambda(void)
{
	static const double x[] = { 0, 1, 2, 3, 4 }, y[] = { 1, 3, 5, 7, 9 };
	static const double nx[] = { 0, 1, 2, 3, 4, 5 };
	static const double ny[] = { 1, 2, 1.5, 3, 2.5, 4 };
	static const double fl[] = { -40, -10, 0, 5, 30, 100 };
	static const double zf[] = { -100, -20, 15, 60 };
	double s, i, r2;
	sig_lambda_t l;
	sig_fit_t f;
	uint32_t k;

	sig_linreg(x, y, 5, &s, &i, &r2);
	CHECK(approx(s, 2) && approx(i, 1) && approx(r2, 1));
	sig_linreg(nx, ny, 6, &s, &i, &r2);
	CHECK(r2 > 0 && r2 < 1);
	sig_linreg(x, y, 1, &s, &i, &r2);
	CHECK(r2 == 0);
	sig_linreg((const double[]){ 2, 2, 2 }, (const double[]){ 1, 2, 3 }, 3,
	    &s, &i, &r2);
	CHECK(r2 == 0 && approx(i, 2));
	sig_linreg((const double[]){ 1, 2, 3 }, (const double[]){ 5, 5, 5 }, 3,
	    &s, &i, &r2);
	CHECK(r2 == 0);
	/* RecoversKnownLambda: dP = 0.25 y exactly. */
	memset(&l, 0, sizeof l);
	for (k = 0; k < 6; k++)
		sig_lambda_add(&l, fl[k], 0.25 * fl[k]);
	f = sig_lambda_fit(&l);
	CHECK(approx(f.lambda, 0.25) && approx(f.r2, 1) && f.n == 6);
	/* NoImpactGivesZeroLambda. */
	memset(&l, 0, sizeof l);
	for (k = 0; k < 4; k++)
		sig_lambda_add(&l, zf[k], 0);
	f = sig_lambda_fit(&l);
	CHECK(f.lambda == 0 && f.r2 == 0);
	/* The window keeps the newest SIG_LAMBDA_WINDOW pairs, in order:
	 * 300 pairs, the first 60 with slope 9, the rest slope 2. */
	memset(&l, 0, sizeof l);
	for (k = 0; k < 300; k++)
		sig_lambda_add(&l, (double)k, (k < 60 ? 9.0 : 2.0) * k);
	f = sig_lambda_fit(&l);
	CHECK(f.n == SIG_LAMBDA_WINDOW && approx(f.lambda, 2) &&
	    approx(f.r2, 1));
}

static sv_event_t
ev(uint8_t kind, uint64_t seq, uint32_t user, uint64_t order, int64_t qty)
{
	sv_event_t e;

	memset(&e, 0, sizeof e);
	e.kind = kind;
	e.seq = seq;
	e.user = user;
	e.order = order;
	e.qty = qty;
	return e;
}

static sv_event_t
tr(uint64_t seq, uint64_t maker, uint64_t taker, int64_t qty)
{
	sv_event_t e = ev(SV_TRADE, seq, 0, 0, qty);

	e.maker = maker;
	e.taker = taker;
	return e;
}

static sv_monitor_t mon;

static void
mkmon(int64_t min_size, uint64_t lifetime, uint64_t window,
    uint32_t min_orders, double ratio)
{
	sv_config_t c;

	memset(&c, 0, sizeof c);
	c.min_size = min_size;
	c.max_lifetime = lifetime;
	c.otr_window = window;
	c.otr_min_orders = min_orders ? min_orders : 1u << 30;
	c.otr_max_ratio = ratio;
	sv_init(&mon, &c);
}

/* surveillance_test.go: SpoofDetector{MinSize 50, MaxLifetime 5}. */
static void
t_spoof(void)
{
	sv_event_t e;
	const sv_alert_t *a;

	mkmon(50, 5, 100, 0, 0);
	e = ev(SV_PLACED, 1, 7, 1, 100);
	CHECK(sv_observe(&mon, &e) == 0);
	e = ev(SV_CANCELLED, 3, 7, 1, 0);
	CHECK(sv_observe(&mon, &e) == 1);
	a = sv_alert(&mon, 0);
	CHECK(a != NULL && a->kind == SV_SPOOFING && a->user == 7 &&
	    a->a == 1 && a->b == 100 && a->c == 2);
	/* IgnoresFilled: a partial fill is real liquidity. */
	mkmon(50, 5, 100, 0, 0);
	e = ev(SV_PLACED, 1, 8, 1, 100);
	(void)sv_observe(&mon, &e);
	e = tr(2, 1, 99, 10);
	(void)sv_observe(&mon, &e);
	e = ev(SV_CANCELLED, 3, 8, 1, 0);
	CHECK(sv_observe(&mon, &e) == 0);
	/* IgnoresSmallAndSlow. */
	mkmon(50, 5, 100, 0, 0);
	e = ev(SV_PLACED, 1, 9, 1, 10);
	(void)sv_observe(&mon, &e);
	e = ev(SV_CANCELLED, 2, 9, 1, 0);
	CHECK(sv_observe(&mon, &e) == 0);
	e = ev(SV_PLACED, 1, 9, 2, 100);
	(void)sv_observe(&mon, &e);
	e = ev(SV_CANCELLED, 10, 9, 2, 0);
	CHECK(sv_observe(&mon, &e) == 0);
	/* AFilledOrderCancelledLaterRaisesNothing; a fully filled order is
	 * forgotten (nothing left to watch). */
	mkmon(50, 5, 100, 0, 0);
	e = ev(SV_PLACED, 1, 9, 3, 100);
	(void)sv_observe(&mon, &e);
	e = tr(2, 3, 50, 100);
	(void)sv_observe(&mon, &e);
	CHECK(mon.n_ord == 0);
	e = ev(SV_CANCELLED, 3, 9, 3, 0);
	CHECK(sv_observe(&mon, &e) == 0);
}

/* surveillance_test.go: OTRDetector{Window 100, MinOrders 5, MaxRatio 3}. */
static void
t_otr(void)
{
	sv_event_t e;
	const sv_alert_t *a;
	uint64_t id;
	uint32_t last = 0;

	/* FlagsManyOrdersFewFills: ten placements, one fill, then OTR 11/1. */
	mkmon(1 << 30, 0, 100, 5, 3.0);
	for (id = 1; id <= 10; id++) {
		e = ev(SV_PLACED, id, 5, id, 1);
		(void)sv_observe(&mon, &e);
	}
	e = tr(11, 1, 99, 1);
	(void)sv_observe(&mon, &e);
	e = ev(SV_PLACED, 12, 5, 12, 1);
	CHECK(sv_observe(&mon, &e) == 1);
	a = sv_alert(&mon, mon.n_alerts - 1);
	CHECK(a->kind == SV_OTR && a->user == 5 && a->a == 11 && a->b == 1 &&
	    approx(a->ratio, 11));
	/* QuietWhenTrading: six placements, six fills. */
	mkmon(1 << 30, 0, 100, 5, 3.0);
	for (id = 1; id <= 6; id++) {
		e = ev(SV_PLACED, id * 2 - 1, 6, id, 1);
		last = sv_observe(&mon, &e);
		e = tr(id * 2, id, 99, 1);
		(void)sv_observe(&mon, &e);
	}
	CHECK(last == 0 && mon.n_alerts == 0);
	/* NeedsMinimumSample: four placements, no fills. */
	mkmon(1 << 30, 0, 100, 5, 3.0);
	for (id = 1; id <= 4; id++) {
		e = ev(SV_PLACED, id, 4, id, 1);
		CHECK(sv_observe(&mon, &e) == 0);
	}
	/* Window: placements older than seq - Window drop out. */
	mkmon(1 << 30, 0, 10, 5, 3.0);
	for (id = 1; id <= 4; id++) {
		e = ev(SV_PLACED, id, 4, id, 1);
		(void)sv_observe(&mon, &e);
	}
	e = ev(SV_PLACED, 100, 4, 100, 1);
	CHECK(sv_observe(&mon, &e) == 0);
}

/* Go's math/rand: regress_test.go (seed 0, Int63) and `go run`. */
static void
t_gorand(void)
{
	gorand_t g;

	gorand_seed(&g, 0);
	CHECK(gorand_int63(&g) == INT64_C(8717895732742165505));
	CHECK(gorand_int63(&g) == INT64_C(2259404117704393152));
	CHECK(gorand_int63(&g) == INT64_C(6050128673802995827));
	gorand_seed(&g, 1);
	CHECK(gorand_int63(&g) == INT64_C(5577006791947779410));
	CHECK(gorand_int63(&g) == INT64_C(8674665223082153551));
	CHECK(gorand_int31n(&g, 50) == 47);
	CHECK(gorand_int31n(&g, 1901) == 391);
	CHECK(gorand_float64(&g) == 0.4246374970712657);
	CHECK(gorand_int31n(&g, 7) == 4);
	CHECK(gorand_int31n(&g, 64) == 57);
	gorand_seed(&g, -7);
	CHECK(gorand_int63(&g) == INT64_C(747107023976529931));
}

static con_t con;
static void *con_mem;

static void
mkcon(int64_t seed)
{
	size_t n = con_book_bytes();

	if (con_mem == NULL && posix_memalign(&con_mem, 64, n) != 0)
		abort();
	if (con_init(&con, con_mem, n, seed) != 0)
		abort();
}

/*
 * Their console, headless (cmd/obwasm built with Go 1.26.10, obReset(1),
 * obStep(1) x 79, obSignals / obSnapshot(10)): mid 100.00, spread 0.01,
 * OFI -6.158, CVD -0.606, imbalance -0.3374369169597798, lambda 0.2146...
 * per unit (R^2 0.0601..., n 71), 28 prints, and this ladder.
 */
static void
t_console_parity(void)
{
	static const int64_t bids[][2] = { { 10000, 1865 }, { 9999, 235 },
		{ 9998, 3611 }, { 9997, 2272 }, { 9996, 682 }, { 9995, 2744 },
		{ 9994, 1338 }, { 9993, 1431 } };
	static const int64_t asks[][2] = { { 10001, 3488 }, { 10002, 1600 },
		{ 10003, 5716 }, { 10004, 5957 }, { 10005, 730 }, { 10006, 1058 },
		{ 10007, 1308 }, { 10008, 1269 } };
	con_signals_t s;
	sig_snap_t sn;
	uint32_t i, prints = 0;

	mkcon(1);
	for (i = 0; i < 79; i++)
		prints += con_step(&con, 1);
	s = con_signals(&con);
	con_snapshot(&con, &sn);
	CHECK(prints == 28 && s.step == 79);
	CHECK(s.mid == 10000 && s.spread == 1 && s.last == 10000);
	CHECK(s.ofi == -6158 && s.cvd == -606);
	CHECK(approx(s.imbalance, -0.3374369169597798));
	CHECK(s.lambda.n == 71 &&
	    approx(s.lambda.lambda * 1000, 0.21463660886548466) &&
	    approx(s.lambda.r2, 0.06012877762957247));
	CHECK(sn.nb == 8 && sn.na == 8);
	for (i = 0; i < 8; i++)
		CHECK(sn.bp[i] == bids[i][0] && sn.bq[i] == bids[i][1] &&
		    sn.ap[i] == asks[i][0] && sn.aq[i] == asks[i][1]);
	CHECK(book_check(con.b) == 0);
	/* 1000 steps: still theirs (527 prints, OFI -46.909, CVD -15.761). */
	for (i = 79; i < 1000; i++)
		prints += con_step(&con, 1);
	s = con_signals(&con);
	CHECK(prints == 527 && s.mid == 9993 && s.spread == 3 &&
	    s.ofi == -46909 && s.cvd == -15761 && s.lambda.n == 240 &&
	    approx(s.lambda.lambda * 1000, 0.1262449784967043));
}

/*
 * Their console, seed 1: obSpoof at step 300, obFlood at 600, "you" buy
 * 1.5 @ 99.50 at 900, market sell 2.0 at 950, 2000 steps.  Theirs raises
 * 9 alerts: spoofing on orders 271-273 at seqs 446-448 (10 events after
 * placement), then OTR on the flooder at seqs 892-897 (25..30 orders, 0
 * fills); the "you" order is id 845, the market sell id 890 prints three
 * fills at 99.92 (1.298, 0.657, 0.045).
 */
static void
t_console_provocations(void)
{
	const sv_alert_t *a;
	uint64_t id = 0, ids[4];
	int64_t sd[4], px[4], q[4];
	int i;

	mkcon(1);
	for (i = 0; i < 2000; i++) {
		if (i == 300)
			CHECK(con_spoof(&con) == 0 && con_spoof(&con) == -1);
		if (i == 600)
			CHECK(con_flood(&con) == 0);
		if (i == 900) {
			CHECK(con_submit(&con, CON_YOU, 0, 0, 9950, 1500, &id) == 0);
			CHECK(id == 845);
			CHECK(con_open_orders(&con, CON_YOU, ids, sd, px, q, 4) == 1);
			CHECK(ids[0] == 845 && sd[0] == BOOK_BUY && px[0] == 9950 &&
			    q[0] == 1500);
		}
		if (i == 950) {
			CHECK(con_submit(&con, CON_YOU, 1, 1, 0, 2000, &id) == 0);
			CHECK(id == 890 && con.n_trades == 3);
			CHECK(con.trades[0].price == 9992 && con.trades[0].qty == 1298 &&
			    con.trades[1].qty == 657 && con.trades[2].qty == 45 &&
			    con.trades[0].taker_sold);
		}
		(void)con_step(&con, 1);
	}
	CHECK(con.sv.n_alerts == 9);
	for (i = 0; i < 3; i++) {
		a = sv_alert(&con.sv, (uint64_t)i);
		CHECK(a->kind == SV_SPOOFING && a->user == CON_SPOOFER &&
		    a->seq == 446 + (uint64_t)i && a->a == 271 + i &&
		    a->b == 8000 && a->c == 10);
	}
	for (i = 3; i < 9; i++) {
		a = sv_alert(&con.sv, (uint64_t)i);
		CHECK(a->kind == SV_OTR && a->user == CON_FLOODER &&
		    a->seq == 889 + (uint64_t)i && a->a == 22 + i && a->b == 0);
	}
	CHECK(con_cancel(&con, 845, CON_SPOOFER) == BOOK_R_UNKNOWN_ID);
	CHECK(con_cancel(&con, 845, CON_YOU) == 0);
	CHECK(con_cancel(&con, 845, CON_YOU) == BOOK_R_UNKNOWN_ID);
	CHECK(book_check(con.b) == 0 && con.sv.dropped == 0);
}

/* Same seed, same steps, same digest; another seed differs.  And a long
 * run stays sound with the detectors' tables bounded by what rests. */
static void
t_console_replay(void)
{
	uint64_t d1, d2, d3;
	int i;

	mkcon(42);
	for (i = 0; i < 20000; i++)
		(void)con_step(&con, 1);
	d1 = book_digest(con.b);
	CHECK(book_check(con.b) == 0);
	CHECK(con.sv.n_ord <= con.b->n_live && con.sv.dropped == 0);
	mkcon(42);
	for (i = 0; i < 100; i++)
		(void)con_step(&con, 200);
	d2 = book_digest(con.b);
	mkcon(43);
	for (i = 0; i < 20000; i++)
		(void)con_step(&con, 1);
	d3 = book_digest(con.b);
	CHECK(d1 == d2 && d1 != d3);
	printf("  replay: seed 42, 20000 steps, digest %016" PRIx64
	    " (both ways); seed 43 differs\n", d1);
}

int
main(int argc, char **argv)
{
	static const struct {
		const char *name;
		void (*fn)(void);
	} cases[] = {
		{ "ofi", t_ofi },
		{ "imbalance", t_imbalance },
		{ "linreg_lambda", t_linreg_lambda },
		{ "spoof", t_spoof },
		{ "otr", t_otr },
		{ "gorand", t_gorand },
		{ "console_parity", t_console_parity },
		{ "console_provocations", t_console_provocations },
		{ "console_replay", t_console_replay },
	};
	size_t i;

	(void)argv;
	if (argc > 1) {
		printf("usage: test_signals\n\nThe console's signals, "
		    "surveillance, RNG and market, against known answers.\n");
		return 0;
	}
	for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		int before = failures;
		cases[i].fn();
		printf("%s %s\n", failures == before ? "ok  " : "FAIL",
		    cases[i].name);
	}
	if (failures) {
		printf("test_signals: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_signals: all %zu cases passed\n", i);
	return 0;
}
