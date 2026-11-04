/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/bench_book.c -- the bare engine, one core, no runtime.
 *
 *	What it measures: one in-process call into the matching engine, on
 *	a warm book, for three flows:
 *	  cancel-heavy  ~90 % cancels / 10 % new near the touch (the shape a
 *	                US equity book sees: most orders never trade);
 *	  mixed         60 % rest, 25 % cancel, 15 % marketable;
 *	  sweep         marketable orders walking several levels.
 *	Per-op latency comes from rdtsc-free clock_gettime(CLOCK_MONOTONIC)
 *	pairs, so the absolute floor includes ~20 ns of clock overhead; the
 *	median-to-tail shape is the signal.  Throughput is wall time over
 *	the whole run with no per-op timing.  Nothing here includes a
 *	network, a codec, a session, or a log: it is a floor, not an
 *	end-to-end latency.  The venue benchmark (l2book --bench) adds the
 *	isolate/fiber path.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "book.h"
#include "hist.h"

static uint64_t rng = 0x2545f4914f6cdd1dull;

static inline uint64_t
rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

static inline uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define LIVE_MAX 100000

static book_event_t evbuf[1024];

typedef struct {
	const char *name;
	int pct_cancel, pct_market;
	int spread;		/* new orders land within +-spread ticks */
} flow_t;

static void
run(const flow_t *f, long ops, int timed, hist_t *h, double *mops)
{
	book_config_t cfg;
	book_t *b;
	void *mem;
	static uint64_t live[LIVE_MAX];
	int nlive = 0;
	uint64_t id = 1, t0, t1;
	book_out_t out = { evbuf, 1024, 0, 0 };
	long i;

	memset(&cfg, 0, sizeof cfg);
	cfg.max_orders = 1u << 18;
	cfg.tick = 1;
	cfg.ref_price = 10000;
	if (posix_memalign(&mem, 64, book_bytes(&cfg)) != 0 ||
	    book_init(&b, mem, book_bytes(&cfg), &cfg) != 0)
		abort();
	/* Warm book: 20k resting orders across 200 levels each side. */
	while (nlive < 20000) {
		uint64_t r = rnd();
		int side = (int)(r & 1);
		book_order_t o;
		memset(&o, 0, sizeof o);
		o.id = id++;
		o.side = (uint8_t)side;
		o.price = side == BOOK_BUY ? 9999 - (int64_t)((r >> 1) % 200) :
		    10001 + (int64_t)((r >> 1) % 200);
		o.qty = 100 * (1 + (int64_t)((r >> 9) % 5));
		out.n = 0;
		book_submit(b, &o, 0, &out);
		live[nlive++] = o.id;
	}
	t0 = now_ns();
	for (i = 0; i < ops; i++) {
		uint64_t r = rnd();
		int op = (int)(r % 100);
		uint64_t s = 0;
		out.n = 0;
		if (op < f->pct_cancel && nlive > 0) {
			int k = (int)((r >> 8) % (uint64_t)nlive);
			if (timed)
				s = now_ns();
			book_cancel(b, live[k], 0, &out);
			if (timed)
				hist_record(h, now_ns() - s);
			live[k] = live[--nlive];
		} else {
			book_order_t o;
			int side = (int)((r >> 8) & 1);
			int64_t bid = book_best(b, BOOK_BUY), ask = book_best(b,
			    BOOK_SELL);
			memset(&o, 0, sizeof o);
			o.id = id++;
			o.side = (uint8_t)side;
			o.qty = 100 * (1 + (int64_t)((r >> 9) % 5));
			if (op < f->pct_cancel + f->pct_market) {
				o.type = BOOK_LIMIT;
				o.tif = BOOK_IOC;
				o.price = side == BOOK_BUY ? (ask == BOOK_NO_PRICE ?
				    10000 : ask + f->spread) : (bid == BOOK_NO_PRICE ?
				    10000 : bid - f->spread);
			} else {
				int64_t off = (int64_t)((r >> 12) %
				    (uint64_t)(f->spread + 1));
				o.price = side == BOOK_BUY ? (bid == BOOK_NO_PRICE ?
				    9999 : bid - off) : (ask == BOOK_NO_PRICE ?
				    10001 : ask + off);
			}
			if (timed)
				s = now_ns();
			book_submit(b, &o, 0, &out);
			if (timed)
				hist_record(h, now_ns() - s);
			if (o.tif != BOOK_IOC && nlive < LIVE_MAX &&
			    book_find(b, o.id) != NULL)
				live[nlive++] = o.id;
		}
		/* Ids of orders that traded away are cancelled as unknown,
		 * which is cheap and realistic (a cancel racing a fill). */
	}
	t1 = now_ns();
	*mops = (double)ops / ((double)(t1 - t0) / 1e3);
	if (book_check(b) != 0) {
		fprintf(stderr, "bench_book: book_check failed\n");
		exit(1);
	}
	free(mem);
}

int
main(int argc, char **argv)
{
	static const flow_t flows[] = {
		{ "cancel-heavy", 90, 2, 3 },
		{ "mixed", 25, 15, 5 },
		{ "sweep", 10, 40, 8 },
	};
	long ops = 2000000;
	size_t i;

	if (argc > 1 && strcmp(argv[1], "--help") == 0) {
		printf("usage: bench_book [OPS]\n\nThe bare matching engine on "
		    "one core: per-op latency and throughput, three flows.\n");
		return 0;
	}
	if (argc > 1)
		ops = atol(argv[1]);
	printf("%-13s %9s %7s %7s %7s %7s %8s\n", "flow", "Mops/s", "p50",
	    "p99", "p999", "p9999", "max(ns)");
	for (i = 0; i < sizeof flows / sizeof flows[0]; i++) {
		static hist_t h;
		double mops_t, mops;
		memset(&h, 0, sizeof h);
		run(&flows[i], ops, 1, &h, &mops_t);
		run(&flows[i], ops, 0, &h, &mops);	/* untimed: throughput */
		printf("%-13s %9.2f %7" PRIu64 " %7" PRIu64 " %7" PRIu64
		    " %7" PRIu64 " %8" PRIu64 "\n", flows[i].name, mops,
		    hist_q(&h, 0.50), hist_q(&h, 0.99), hist_q(&h, 0.999),
		    hist_q(&h, 0.9999), h.max);
	}
	return 0;
}
