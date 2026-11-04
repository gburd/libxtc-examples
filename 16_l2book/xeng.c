/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/xeng.c -- this engine as an adapter in intrepidkarthi/orderbook's
 * cross-engine comparison (its docs/CROSS-ENGINE.md, protocol in its
 * internal/benchgate/xeng.go).
 *
 *	stdin:   N <count>
 *	         S <pos> <acct> <side> <price> <qty>   limit GTC, side 0 buy
 *	         C <pos> <acct> <target>               cancel the order made at
 *	                                               position target
 *	stdout:  c <pos> <refused>                     one per command
 *	         X <price> <qty> <maker> <taker> <B|S> that command's trades
 *	         L <pos> <side> <price> <qty> <filled> the final book, in
 *	                                               priority order
 *	         E <last trade price> <trade count>
 *	         T <replay ns>
 *
 *	Same rules as the other adapters: input parsed and orders built before
 *	the clock starts; inside the timed loop only book_submit/book_cancel
 *	and recording each command's outcome and trades into preallocated
 *	arrays; ids map to positions after the clock stops.  The order made at
 *	position pos gets id pos + 1 (the engine reserves id 0), so a cancel
 *	of a position that made no order names an id the engine never saw and
 *	is refused.  Self-trade prevention is ALLOW: the tape has no account
 *	that meets itself, and ALLOW does no account check on the match path.
 *	The book is sized for the tape (the pool holds every submit), the
 *	equivalent of the Go engine's PreallocOrders and geseq's prefilled
 *	pool; book_init touches all of it before the clock starts.
 *
 *	-r R: replay the tape R times, each in a fresh book, and report the
 *	median on T.  The protocol lines are the last run's; every run's
 *	final book digest must equal the first's or the adapter exits 1.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "book.h"

struct cmd {
	int64_t	price, qty;
	int64_t	target;	/* cancels */
	uint32_t acct;
	uint8_t	cancel, sell;
};

struct trade {
	int64_t	price, qty;
	uint64_t maker, taker;	/* engine ids (pos + 1) */
	uint8_t	sell;		/* aggressor (taker) side */
};

static struct cmd *cmds;
static long ncmd;
static struct trade *trades;
static long ntrade, trade_cap;
static uint32_t *ends;		/* trades[ends[i-1]..ends[i]) are command i's */
static uint8_t *refused;

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void
die(const char *m)
{
	fprintf(stderr, "l2book-xeng: %s\n", m);
	exit(1);
}

static void
parse(FILE *in)
{
	char line[256];
	long n = 0;
	if (fgets(line, sizeof line, in) == NULL ||
	    sscanf(line, "N %ld", &n) != 1 || n <= 0)
		die("first line is not N <count>");
	cmds = calloc((size_t)n, sizeof *cmds);
	ends = calloc((size_t)n, sizeof *ends);
	refused = calloc((size_t)n, sizeof *refused);
	if (cmds == NULL || ends == NULL || refused == NULL)
		die("out of memory");
	while (fgets(line, sizeof line, in) != NULL) {
		long pos, acct, a, b, c;
		struct cmd *x;
		if (line[0] == 'S' &&
		    sscanf(line + 1, "%ld %ld %ld %ld %ld", &pos, &acct, &a, &b,
		    &c) == 5) {
			if (pos != ncmd || pos >= n)
				die("command out of order");
			x = &cmds[ncmd++];
			x->acct = (uint32_t)acct;
			x->sell = (uint8_t)(a != 0);
			x->price = b;
			x->qty = c;
		} else if (line[0] == 'C' &&
		    sscanf(line + 1, "%ld %ld %ld", &pos, &acct, &a) == 3) {
			if (pos != ncmd || pos >= n)
				die("command out of order");
			x = &cmds[ncmd++];
			x->cancel = 1;
			x->acct = (uint32_t)acct;
			x->target = a;
		} else if (line[0] != '\n')
			die("not a protocol line");
	}
	if (ncmd != n)
		die("fewer commands than N");
}

static int64_t g_lo = INT64_MAX, g_hi = INT64_MIN;

static void
book_for_tape(book_t **b, void **mem)
{
	book_config_t cfg;
	long i, submits = 0;
	size_t len;

	memset(&cfg, 0, sizeof cfg);
	for (i = 0; i < ncmd; i++)
		submits += !cmds[i].cancel;
	cfg.max_orders = (uint32_t)submits + 16;
	cfg.tick = 1;
	cfg.ref_price = (g_lo + g_hi) / 2;
	cfg.band_pct_bp = 0;		/* no circuit breaker: plain matching */
	len = book_bytes(&cfg);
	if (posix_memalign(mem, 64, len) != 0)
		die("out of memory");
	memset(*mem, 0, len);		/* fault every page in before timing */
	if (book_init(b, *mem, len, &cfg) != 0)
		die("book_init");
}

/* One replay; returns its wall time.  Records into the global arrays. */
static uint64_t
replay(book_t *b, book_out_t *out)
{
	book_order_t o;
	uint64_t t0, t1;
	long i;
	uint32_t k;

	ntrade = 0;
	memset(&o, 0, sizeof o);
	o.type = BOOK_LIMIT;
	o.tif = BOOK_GTC;
	o.stp = BOOK_STP_ALLOW;
	t0 = now_ns();
	for (i = 0; i < ncmd; i++) {
		const struct cmd *c = &cmds[i];
		out->n = 0;
		if (c->cancel) {
			book_cancel(b, (uint64_t)c->target + 1, c->acct, out);
		} else {
			o.id = (uint64_t)i + 1;
			o.account = c->acct;
			o.side = c->sell ? BOOK_SELL : BOOK_BUY;
			o.price = c->price;
			o.qty = c->qty;
			book_submit(b, &o, 0, out);
		}
		refused[i] = 0;
		for (k = 0; k < out->n; k++) {
			const book_event_t *e = &out->ev[k];
			if (e->kind == BOOK_EV_REJECTED)
				refused[i] = 1;
			else if (e->kind == BOOK_EV_TRADE) {
				struct trade *t = &trades[ntrade++];
				t->price = e->price;
				t->qty = e->qty;
				t->maker = e->other;
				t->taker = e->id;
				t->sell = e->side == BOOK_SELL;
			}
		}
		ends[i] = (uint32_t)ntrade;
	}
	t1 = now_ns();
	return t1 - t0;
}

struct emit {
	FILE *w;
};

static void
emit_resting(const book_slot_t *s, void *arg)
{
	struct emit *e = arg;
	long pos = (long)s->id - 1;
	fprintf(e->w, "L %ld %d %lld %lld %lld\n", pos, s->side == BOOK_SELL,
	    (long long)s->price, (long long)cmds[pos].qty,
	    (long long)(cmds[pos].qty - s->leaves));
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

int
main(int argc, char **argv)
{
	static book_event_t evbuf[4096];
	book_out_t out;
	book_t *b = NULL;
	void *mem = NULL;
	uint64_t *ns, digest0 = 0;
	struct emit em;
	long i, from = 0;
	int r, runs = 1;

	if (argc == 3 && strcmp(argv[1], "-r") == 0)
		runs = atoi(argv[2]);
	else if (argc != 1) {
		fprintf(stderr, "usage: l2book-xeng [-r RUNS] < tape.txt\n");
		return 2;
	}
	if (runs < 1)
		runs = 1;
	parse(stdin);
	for (i = 0; i < ncmd; i++)
		if (!cmds[i].cancel) {
			if (cmds[i].price < g_lo)
				g_lo = cmds[i].price;
			if (cmds[i].price > g_hi)
				g_hi = cmds[i].price;
		}
	if (g_hi - g_lo >= BOOK_LADDER / 2)
		die("tape's price range is wider than this adapter's ladder");
	/* A command trades at most once per resting order, and every order
	 * rests at most once: trades < commands. */
	trade_cap = ncmd + 1;
	trades = calloc((size_t)trade_cap, sizeof *trades);
	ns = calloc((size_t)runs, sizeof *ns);
	if (trades == NULL || ns == NULL)
		die("out of memory");
	out.ev = evbuf;
	out.cap = sizeof evbuf / sizeof evbuf[0];
	for (r = 0; r < runs; r++) {
		if (b != NULL)
			free(mem);
		book_for_tape(&b, &mem);
		out.overflow = 0;
		ns[r] = replay(b, &out);
		if (out.overflow)
			die("event buffer overflow");
		if (book_check(b) != 0)
			die("book_check failed after the replay");
		if (r == 0)
			digest0 = book_digest(b);
		else if (book_digest(b) != digest0)
			die("runs disagree");
	}
	qsort(ns, (size_t)runs, sizeof *ns, cmp_u64);

	em.w = stdout;
	for (i = 0; i < ncmd; i++) {
		long t;
		printf("c %ld %d\n", i, refused[i]);
		for (t = from; t < (long)ends[i]; t++)
			printf("X %lld %lld %llu %llu %c\n",
			    (long long)trades[t].price, (long long)trades[t].qty,
			    (unsigned long long)(trades[t].maker - 1),
			    (unsigned long long)(trades[t].taker - 1),
			    trades[t].sell ? 'S' : 'B');
		from = (long)ends[i];
	}
	book_foreach_resting(b, emit_resting, &em);
	printf("E %lld %ld\nT %llu\n",
	    (long long)(b->last_price == BOOK_NO_PRICE ? 0 : b->last_price),
	    ntrade, (unsigned long long)ns[runs / 2]);
	if (runs > 1)
		fprintf(stderr, "l2book-xeng: %d runs: min %llu median %llu max "
		    "%llu ns\n", runs, (unsigned long long)ns[0],
		    (unsigned long long)ns[runs / 2],
		    (unsigned long long)ns[runs - 1]);
	free(mem);
	return 0;
}
