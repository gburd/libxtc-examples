/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/console.h -- the browser console's market: book.c matching a
 * simulated noise-trader flow, with the console's signals and
 * surveillance watching every event.  Pure C, no libc, no allocation:
 * compiled into book.wasm for the page, and natively for test_console.
 *
 *	The market is intrepidkarthi/orderbook's console market (cmd/obwasm
 *	at 3fffc25, MIT), rebuilt on this engine: one sim.NoiseTrader with
 *	the parameters their console sets (50 identities, acts with
 *	probability 0.9, 30% market orders, limits 0..6 ticks off the
 *	reference, 100..2000 lots of 0.001), the same instrument (0.01 tick,
 *	0.001 lot, $100.00 start), the same spoof and flood provocations and
 *	detector thresholds, and Go's math/rand (gorand.c) seeded the same
 *	way, so seed N draws the orders seed N draws there.
 */
#ifndef L2BOOK_CONSOLE_H
#define L2BOOK_CONSOLE_H

#include "book.h"
#include "gorand.h"
#include "signals.h"

#define CON_START	10000	/* $100.00 in cent ticks */
#define CON_MAX_ORDERS	100000	/* their DefaultConfig MaxOrders */
#define CON_DEPTH	10	/* bookDepth */
#define CON_LAMBDA_MIN	30	/* lambdaMinN: below it the fit is noise */
#define CON_TRADES	4096	/* trades kept from the last call */
#define CON_EVENTS	65536

/* Accounts.  The noise identities "flow-0".."flow-49" are CON_FLOW + n. */
enum {
	CON_YOU = 1,
	CON_SPOOFER = 2,
	CON_FLOODER = 3,
	CON_FLOW = 100
};

typedef struct {
	int64_t		price;		/* ticks */
	int64_t		qty;		/* lots */
	uint32_t	taker_sold;	/* 1 = the taker sold */
	uint32_t	_pad;
} con_trade_t;

typedef struct {
	book_t		*b;
	gorand_t	rng;
	int64_t		step;
	uint64_t	seq;		/* surveillance event sequence */
	uint64_t	next_id;
	sig_ofi_t	ofi;
	int64_t		cvd;		/* lots */
	sig_lambda_t	lam;
	sv_monitor_t	sv;
	uint64_t	spoof_ids[3];
	uint32_t	n_spoof;
	int64_t		spoof_at;
	con_trade_t	trades[CON_TRADES];
	uint32_t	n_trades;	/* in trades[] from the last call */
	uint64_t	trades_more;	/* printed but not kept (buffer full) */
	uint64_t	refused;	/* orders the engine refused */
	uint64_t	overflow;	/* engine events that did not fit */
	book_event_t	ev[CON_EVENTS];
	book_out_t	out;
} con_t;

typedef struct {
	int64_t		step;
	int		has_mid;
	int64_t		mid;		/* ticks, floored: (bid + ask) / 2 */
	int64_t		spread;		/* ticks */
	int64_t		last;		/* ticks, 0 = none yet */
	int64_t		ofi;		/* cumulative, lots */
	int64_t		cvd;		/* lots */
	double		imbalance;	/* top 5 */
	double		best_imbalance;
	sig_fit_t	lambda;		/* n = 0 while warming up */
} con_signals_t;

/* Book memory (64-byte aligned) the console needs. */
size_t	con_book_bytes(void);
/* obReset(seed): a fresh market.  0, or -1 for short / unaligned mem. */
int	con_init(con_t *c, void *mem, size_t len, int64_t seed);
/* obStep(n): n steps (1..200); the trades land in c->trades. */
uint32_t con_step(con_t *c, int n);
/*
 * obSubmit: a limit (market = 0, GTC) or market (IOC) order for account,
 * through the same observed path as the flow.  Returns 0, or the
 * book_reason_t that refused it (BOOK_R_BAD_QTY / BOOK_R_BAD_PRICE
 * without reaching the engine, as their NewOrder validation does).  The
 * trades land in c->trades; *id is the order id (0 if not entered).
 */
int	con_submit(con_t *c, uint32_t account, int side, int market,
	    int64_t price, int64_t qty, uint64_t *id);
/* obCancel: ownership enforced by the engine.  0 or BOOK_R_UNKNOWN_ID. */
int	con_cancel(con_t *c, uint64_t id, uint32_t account);
/* obSpoof: 0; -1 a spoof is already resting; -2 no bid yet; -3 refused. */
int	con_spoof(con_t *c);
/* obFlood: 0, or -2 no bid yet. */
int	con_flood(con_t *c);
void	con_snapshot(const con_t *c, sig_snap_t *s);
con_signals_t con_signals(const con_t *c);
/* obOpenOrders: account's resting orders in book order; returns count. */
uint32_t con_open_orders(const con_t *c, uint32_t account, uint64_t *id,
	    int64_t *side, int64_t *price, int64_t *qty, uint32_t cap);

#endif /* L2BOOK_CONSOLE_H */
