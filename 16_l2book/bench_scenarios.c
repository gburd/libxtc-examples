/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/bench_scenarios.c -- intrepidkarthi/orderbook's per-operation
 * latency scenarios (pkg/matching/latency_scenarios_test.go and
 * latency_test.go), ported op for op to this engine, so the two
 * distributions are of the same work.
 *
 *	Same preloads, same price formulas, same order sizes, same refills;
 *	every op timed alone with one monotonic clock pair (theirs: time.Now
 *	+ time.Since around the call), sorted, nearest-rank quantiles.  The
 *	accounts mirror theirs ("mm", "tk", "self"); STP is the engine's
 *	default for theirs (Config{}: ALLOW) except STPSweep, which runs all
 *	five modes as they do.  Each scenario's book is centred on its price
 *	range; every range fits this engine's 4096-tick ladder except
 *	ThinBook's 5000 levels, whose top 904 are refused (BOOK_R_BAD_PRICE;
 *	a US-equity book never spans $50 at a penny tick) -- the "refused"
 *	column counts every refusal, so nothing hides in it.
 *
 *	usage: bench_scenarios [N]   (default 200000, their -benchtime=200000x)
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "book.h"

#define MM	1u
#define TK	2u
#define SELF	3u
#define VICTIM	4u

static book_event_t g_ev[1 << 14];
static book_out_t g_out;
static uint64_t g_next_id = 1;
static uint64_t *g_lat;
static long g_n;

static inline uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static long g_refused;

static void
count_refused(void)
{
	uint32_t k;
	for (k = 0; k < g_out.n; k++)
		if (g_ev[k].kind == BOOK_EV_REJECTED)
			g_refused++;
}

/* ref = the centre of the scenario's price range: the 4096-tick ladder
 * spans ref +- 2048, and every scenario's prices fit inside it. */
static book_t *
mkbook(uint32_t max_orders, int64_t ref, void **mem)
{
	book_config_t cfg;
	book_t *b = NULL;
	size_t len;

	memset(&cfg, 0, sizeof cfg);
	cfg.max_orders = max_orders;
	cfg.tick = 1;
	cfg.ref_price = ref;
	len = book_bytes(&cfg);
	if (posix_memalign(mem, 64, len) != 0)
		exit(1);
	memset(*mem, 0, len);
	if (book_init(&b, *mem, len, &cfg) != 0)
		exit(1);
	return b;
}

static uint64_t
submit(book_t *b, uint32_t acct, int side, int64_t price, int64_t qty,
    int stp)
{
	book_order_t o;
	memset(&o, 0, sizeof o);
	o.id = g_next_id++;
	o.account = acct;
	o.side = (uint8_t)side;
	o.type = BOOK_LIMIT;
	o.tif = BOOK_GTC;
	o.stp = (uint8_t)stp;
	o.price = price;
	o.qty = qty;
	g_out.n = 0;
	book_submit(b, &o, 0, &g_out);
	count_refused();
	return o.id;
}

/* A timed submit/cancel: the clock pair brackets only the engine call. */
#define TIMED(expr)							\
	do {								\
		uint64_t t0_ = now_ns();				\
		expr;							\
		g_lat[g_n++] = now_ns() - t0_;				\
	} while (0)

static void
timed_submit(book_t *b, uint32_t acct, int side, int64_t price, int64_t qty,
    int stp, uint64_t *id)
{
	book_order_t o;
	memset(&o, 0, sizeof o);
	o.id = g_next_id++;
	o.account = acct;
	o.side = (uint8_t)side;
	o.type = BOOK_LIMIT;
	o.tif = BOOK_GTC;
	o.stp = (uint8_t)stp;
	o.price = price;
	o.qty = qty;
	g_out.n = 0;
	TIMED(book_submit(b, &o, 0, &g_out));
	count_refused();
	if (id != NULL)
		*id = o.id;
}

static void
timed_cancel(book_t *b, uint64_t id, uint32_t acct)
{
	g_out.n = 0;
	TIMED(book_cancel(b, id, acct, &g_out));
	count_refused();
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

/* Nearest rank, as their pctl: sorted[int(q * (n-1))]. */
static uint64_t
q(double p)
{
	return g_lat[(long)(p * (double)(g_n - 1))];
}

static void
report(const char *name, long preload)
{
	qsort(g_lat, (size_t)g_n, sizeof *g_lat, cmp_u64);
	printf("%-22s %8ld %7" PRIu64 " %7" PRIu64 " %7" PRIu64 " %8" PRIu64
	    " %9" PRIu64 " %8ld\n", name, preload, q(0.50), q(0.99), q(0.999),
	    q(0.9999), g_lat[g_n - 1], g_refused);
	g_refused = 0;
}

static book_t *
warm(long n, long extra, void **mem, uint64_t *ids)
{
	book_t *b = mkbook((uint32_t)(n + extra + 16), 3000, mem);
	long i;
	for (i = 0; i < n; i++) {
		uint64_t id = submit(b, MM, BOOK_BUY, 1000 + i % 4000, 1,
		    BOOK_STP_ALLOW);
		if (ids != NULL)
			ids[i] = id;
	}
	return b;
}

int
main(int argc, char **argv)
{
	long N = argc > 1 ? atol(argv[1]) : 200000, i, j, nlive;
	uint64_t *ids, *live;
	void *mem;
	book_t *b;
	static const struct {
		const char *name;
		int stp;
	} stps[] = {
		{ "STPSweep/Allow", BOOK_STP_ALLOW },
		{ "STPSweep/CancelNewest", BOOK_STP_CANCEL_NEWEST },
		{ "STPSweep/CancelOldest", BOOK_STP_CANCEL_OLDEST },
		{ "STPSweep/CancelBoth", BOOK_STP_CANCEL_BOTH },
		{ "STPSweep/Decrement", BOOK_STP_DECREMENT },
	};
	unsigned k;

	g_out.ev = g_ev;
	g_out.cap = sizeof g_ev / sizeof g_ev[0];
	g_lat = calloc((size_t)N + 16, sizeof *g_lat);
	ids = calloc((size_t)(N + 1000) * 2, sizeof *ids);
	live = calloc((size_t)(N + 20000) * 2, sizeof *live);
	if (g_lat == NULL || ids == NULL || live == NULL)
		return 1;
	printf("%-22s %8s %7s %7s %7s %8s %9s %8s   (ns)\n", "scenario",
	    "preload", "p50", "p99", "p99.9", "p99.99", "max", "refused");

	/* CancelHeavy (latency_test.go): warm 20000; 10% new at the next
	 * price, 90% cancel the newest live (their "round-robin" is LIFO). */
	b = warm(20000, N, &mem, live);
	nlive = 20000;
	g_n = 0;
	for (i = 0, j = 0; i < N; i++) {
		if (i % 10 == 0 || nlive == 0) {
			timed_submit(b, MM, BOOK_BUY, 1000 + (20000 + j) % 4000, 1,
			    BOOK_STP_ALLOW, &live[nlive++]);
			j++;
		} else
			timed_cancel(b, live[--nlive], MM);
	}
	report("CancelHeavy", 20000);
	free(mem);

	/* AddOnly: warm 20000, then passive inserts. */
	b = warm(20000, N, &mem, NULL);
	g_n = 0;
	for (i = 0; i < N; i++)
		timed_submit(b, MM, BOOK_BUY, 1000 + (20000 + i) % 4000, 1,
		    BOOK_STP_ALLOW, NULL);
	report("AddOnly", 20000);
	free(mem);

	/* CancelOnly: preload N+1000, cancel the first N in order. */
	b = warm(N + 1000, 0, &mem, ids);
	g_n = 0;
	for (i = 0; i < N; i++)
		timed_cancel(b, ids[i], MM);
	report("CancelOnly", N + 1000);
	free(mem);

	/* AggressiveWalk: 20000 sells at 2000..2199; 5-lot buys at 2200;
	 * refill 10000 untimed whenever fewer than 100 orders rest. */
	b = mkbook((uint32_t)(20000 + N * 2 + 16), 2100, &mem);
	for (i = 0; i < 20000; i++)
		submit(b, MM, BOOK_SELL, 2000 + i % 200, 1, BOOK_STP_ALLOW);
	g_n = 0;
	for (i = 0; i < N; i++) {
		timed_submit(b, TK, BOOK_BUY, 2200, 5, BOOK_STP_ALLOW, NULL);
		if (b->n_live < 100)
			for (j = 0; j < 10000; j++)
				submit(b, MM, BOOK_SELL, 2000 + j % 200, 1,
				    BOOK_STP_ALLOW);
	}
	report("AggressiveWalk", 20000);
	free(mem);

	/* Mixed_70_20_10: warm 20000; i%10==9 aggressive sell 1 @1000,
	 * 7..8 cancel newest live, else passive buy at the next price. */
	b = warm(20000, N, &mem, live);
	nlive = 20000;
	g_n = 0;
	for (i = 0; i < N; i++) {
		if (i % 10 == 9)
			timed_submit(b, TK, BOOK_SELL, 1000, 1, BOOK_STP_ALLOW,
			    NULL);
		else if (i % 10 >= 7) {
			if (nlive == 0)
				continue;
			timed_cancel(b, live[--nlive], MM);
		} else
			timed_submit(b, MM, BOOK_BUY, 1000 + (20000 + i) % 4000,
			    1, BOOK_STP_ALLOW, &live[nlive++]);
	}
	report("Mixed_70_20_10", 20000);
	free(mem);

	/* ThinBook: 5000 levels of one lot at 3000..4999; a 10-lot buy at
	 * 3020 walks ~10; refill 4000 untimed below 200 resting. */
	b = mkbook((uint32_t)(5000 + N * 2 + 16), 3000 + BOOK_LADDER / 2, &mem);
	for (i = 0; i < 5000; i++)
		submit(b, MM, BOOK_SELL, 3000 + i, 1, BOOK_STP_ALLOW);
	printf("  (ThinBook: %u of 5000 preload orders rest)\n",
	    (unsigned)b->n_live);
	g_n = 0;
	for (i = 0; i < N; i++) {
		timed_submit(b, TK, BOOK_BUY, 3020, 10, BOOK_STP_ALLOW, NULL);
		if (b->n_live < 200)
			for (j = 0; j < 4000; j++)
				submit(b, MM, BOOK_SELL, 3000 + j, 1,
				    BOOK_STP_ALLOW);
	}
	report("ThinBook", 5000);
	free(mem);

	/* STPSweep: per mode, untimed self maker sell 1@1500, timed self
	 * taker buy 1@1500. */
	for (k = 0; k < sizeof stps / sizeof stps[0]; k++) {
		b = mkbook((uint32_t)(2 * N + 1000), 1500, &mem);
		g_n = 0;
		for (i = 0; i < N; i++) {
			submit(b, SELF, BOOK_SELL, 1500, 1, stps[k].stp);
			timed_submit(b, SELF, BOOK_BUY, 1500, 1, stps[k].stp,
			    NULL);
		}
		report(stps[k].name, 0);
		free(mem);
	}
	(void)VICTIM;
	return 0;
}
