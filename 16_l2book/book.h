/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/book.h -- the matching engine: one limit order book.
 *
 *	A pure C11 core with no libc beyond memset/memcpy, no allocation
 *	after book_init, no floating point, no clock, no globals.  The
 *	server links it into a stackless Isolate (one book per symbol), and
 *	the same file compiles to wasm32 for the browser console, so the
 *	page runs the engine the venue runs.
 *
 *	Prices are int64 ticks, quantities int64 shares.  Every call is a
 *	command; its effects come back as events in a caller-supplied
 *	buffer (book_out_t).  The same command stream produces the same
 *	events and the same digest, byte for byte, on every platform.
 *
 *	Layout (why it is fast):
 *	  - Price ladder: a dense array of levels indexed by price - base,
 *	    over a band of BOOK_LADDER ticks that re-centres when the touch
 *	    walks off it.  A two-level bitmap per side (64 words of 64 bits
 *	    under one summary word) finds the best price and the next level
 *	    with a find-first-set, not a tree walk.
 *	  - Orders: a fixed pool indexed by uint32 (no pointers, so the
 *	    pool is relocatable and wasm-safe), linked into a FIFO per level.
 *	  - Order ids: open-addressed hash, id -> pool index, O(1) cancel.
 *	  - Stops: one sorted array per side, triggered on each trade print.
 */
#ifndef L2BOOK_BOOK_H
#define L2BOOK_BOOK_H

#include <stddef.h>
#include <stdint.h>

#define BOOK_LADDER	4096u	/* ticks in the dense price band; pow2 */
#define BOOK_WORDS	(BOOK_LADDER / 64u)
#define BOOK_NONE	0xffffffffu
#define BOOK_NO_PRICE	INT64_MIN

typedef enum {
	BOOK_BUY = 0,
	BOOK_SELL = 1
} book_side_t;

typedef enum {
	BOOK_LIMIT = 0,
	BOOK_MARKET,
	BOOK_STOP,		/* becomes a market order when triggered */
	BOOK_STOP_LIMIT,	/* becomes a limit order when triggered */
	BOOK_TRAILING_STOP,	/* a stop whose trigger follows the price */
	BOOK_PEGGED		/* priced at entry off bid/ask/mid + offset */
} book_type_t;

typedef enum {
	BOOK_GTC = 0,
	BOOK_IOC,		/* fill what can, cancel the rest */
	BOOK_FOK,		/* fill all at once, or nothing */
	BOOK_DAY,		/* expires at the session close */
	BOOK_GTD		/* expires at expire_ns */
} book_tif_t;

typedef enum {
	BOOK_STP_ALLOW = 0,	/* self-trade allowed */
	BOOK_STP_CANCEL_NEWEST,	/* the incoming order is cancelled */
	BOOK_STP_CANCEL_OLDEST,	/* the resting order is cancelled */
	BOOK_STP_CANCEL_BOTH,
	BOOK_STP_DECREMENT	/* reduce both by the overlap, no trade */
} book_stp_t;

typedef enum {
	BOOK_PEG_BID = 0,
	BOOK_PEG_ASK,
	BOOK_PEG_MID
} book_peg_t;

/* Session phases.  The engine holds no calendar: the venue moves it. */
typedef enum {
	BOOK_PREOPEN = 0,	/* accept, do not match; the book may cross */
	BOOK_CONTINUOUS,
	BOOK_HALTED,		/* accept nothing but cancels */
	BOOK_CANCEL_ONLY,
	BOOK_CLOSED
} book_phase_t;

#define BOOK_F_POST_ONLY	0x01u	/* reject if it would trade on entry */
#define BOOK_F_ALL_OR_NONE	0x02u	/* reserved */

/* An order as submitted. */
typedef struct {
	uint64_t	id;		/* venue order id, unique per book */
	uint32_t	account;	/* owner; 0 = anonymous */
	uint8_t		side;		/* book_side_t */
	uint8_t		type;		/* book_type_t */
	uint8_t		tif;		/* book_tif_t */
	uint8_t		stp;		/* book_stp_t */
	uint8_t		flags;		/* BOOK_F_* */
	uint8_t		peg;		/* book_peg_t, for BOOK_PEGGED */
	int64_t		price;		/* limit price (LIMIT, STOP_LIMIT) */
	int64_t		stop;		/* trigger (STOP, STOP_LIMIT); trail
					 * distance for TRAILING_STOP */
	int64_t		qty;
	int64_t		display;	/* iceberg: shown size; 0 = all */
	int64_t		peg_offset;	/* ticks added to the peg reference */
	uint64_t	oco;		/* one-cancels-other partner id; 0 = none */
	int64_t		expire_ns;	/* GTD deadline (venue clock) */
} book_order_t;

/* Events, in the order they happened. */
typedef enum {
	BOOK_EV_ACCEPTED = 1,	/* rests (or is held as a stop) */
	BOOK_EV_TRADE,		/* a fill: maker id, taker id, price, qty */
	BOOK_EV_CANCELED,	/* by request, IOC rest, STP, OCO, expiry */
	BOOK_EV_REPLACED,	/* replace: new id, price, qty */
	BOOK_EV_REDUCED,	/* reduce in place, priority kept */
	BOOK_EV_REJECTED,
	BOOK_EV_TRIGGERED,	/* a stop became live */
	BOOK_EV_PHASE,		/* a session change */
	BOOK_EV_LEVEL		/* L2: a price level's new total (0 = gone) */
} book_ev_kind_t;

typedef enum {
	BOOK_R_NONE = 0,
	BOOK_R_DUPLICATE_ID,
	BOOK_R_UNKNOWN_ID,
	BOOK_R_BAD_QTY,
	BOOK_R_BAD_PRICE,
	BOOK_R_POST_ONLY_CROSS,
	BOOK_R_FOK_UNFILLABLE,
	BOOK_R_NO_LIQUIDITY,	/* a market order met an empty side */
	BOOK_R_PHASE,		/* not accepted in this session phase */
	BOOK_R_BAND,		/* outside the price band (circuit breaker) */
	BOOK_R_FULL,		/* order pool or id map full */
	BOOK_R_SELF_TRADE,	/* STP cancelled it */
	BOOK_R_EXPIRED,
	BOOK_R_OCO,		/* its partner traded or was cancelled */
	BOOK_R_IOC,		/* IOC remainder */
	BOOK_R_USER		/* cancelled by its owner */
} book_reason_t;

typedef struct {
	uint8_t		kind;		/* book_ev_kind_t */
	uint8_t		side;		/* the order's side (TRADE: taker's) */
	uint8_t		reason;		/* book_reason_t */
	uint8_t		_pad[5];
	uint64_t	id;		/* the order (TRADE: taker) */
	uint64_t	other;		/* TRADE: maker id; REPLACED: new id */
	int64_t		price;
	int64_t		qty;		/* TRADE: fill; LEVEL: new total;
					 * CANCELED/REDUCED: leaves after */
	uint32_t	account;	/* the order's owner (TRADE: taker) */
	uint32_t	maker_account;	/* TRADE */
	uint64_t	seq;		/* book sequence, gap-free */
} book_event_t;

typedef struct {
	book_event_t	*ev;
	uint32_t	cap;
	uint32_t	n;
	uint32_t	overflow;	/* events that did not fit (a bug) */
} book_out_t;

/* One resting order (pool entry). */
typedef struct {
	uint64_t	id;
	uint64_t	oco;
	int64_t		price;
	int64_t		stop;		/* stops: current trigger */
	int64_t		leaves;		/* total remaining */
	int64_t		shown;		/* displayed part now resting */
	int64_t		display;	/* iceberg peak size; 0 = all */
	int64_t		expire_ns;
	int64_t		trail;		/* trailing distance */
	uint32_t	account;
	uint32_t	next;		/* FIFO / free list */
	uint32_t	prev;
	uint8_t		side;
	uint8_t		type;
	uint8_t		tif;
	uint8_t		stp;
	uint8_t		live;		/* 1 resting, 2 stop held */
	uint8_t		_pad[3];
} book_slot_t;

typedef struct {
	uint32_t	head, tail;	/* FIFO of pool indices */
	int64_t		qty;		/* displayed total at this price */
	uint32_t	count;
	uint32_t	_pad;
} book_level_t;

typedef struct {
	book_level_t	lv[BOOK_LADDER];
	uint64_t	bits[BOOK_WORDS];	/* level i non-empty */
	uint64_t	summary;		/* word w non-empty */
	uint32_t	nlevels;
	uint32_t	_pad;
} book_ladder_t;

/* Configuration, fixed at init. */
typedef struct {
	uint32_t	max_orders;	/* pool size (resting + stops) */
	int64_t		tick;		/* price step in ticks (usually 1) */
	int64_t		ref_price;	/* initial band centre and LULD reference */
	int64_t		band_pct_bp;	/* circuit breaker: +- this many basis
					 * points of the reference; 0 = off */
	int		pro_rata;	/* 0 FIFO, 1 pro-rata at each level */
} book_config_t;

typedef struct {
	book_config_t	cfg;
	int64_t		base;		/* price of lv[0] */
	book_ladder_t	side[2];
	/* pool */
	book_slot_t	*pool;
	uint32_t	free_head;
	uint32_t	n_live;
	/* id map: open addressing, cap = 2 * max_orders rounded to pow2 */
	uint64_t	*map_id;
	uint32_t	*map_ix;
	uint32_t	map_mask;
	uint32_t	map_used;	/* live + tombstones */
	/* stops, sorted by trigger: buys ascending, sells descending */
	uint32_t	*stops[2];
	uint32_t	n_stops[2];
	/* state */
	uint8_t		phase;
	uint8_t		_pad[7];
	uint64_t	seq;
	int64_t		last_price;	/* last trade, BOOK_NO_PRICE if none */
	int64_t		ref;		/* circuit-breaker reference */
	uint64_t	trades;
	int64_t		volume;
} book_t;

/*
 * Memory: book_bytes(cfg) bytes, 64-byte aligned, owned by the caller
 * (an Isolate's working memory, a slab, or a wasm static).  book_init
 * lays the book out inside it.  Returns 0, or -1 for a bad cfg / short
 * buffer.
 */
size_t	book_bytes(const book_config_t *cfg);
int	book_init(book_t **out, void *mem, size_t len, const book_config_t *cfg);

/* Commands.  Each appends its events to out (out->n is not reset). */
void	book_submit(book_t *b, const book_order_t *o, int64_t now_ns,
	    book_out_t *out);
void	book_cancel(book_t *b, uint64_t id, uint32_t account,
	    book_out_t *out);
/* Reduce leaves to new_qty (< leaves), keeping queue priority. */
void	book_reduce(book_t *b, uint64_t id, uint32_t account,
	    int64_t new_qty, book_out_t *out);
/* Cancel id and enter new_id at (price, qty): loses priority. */
void	book_replace(book_t *b, uint64_t id, uint32_t account,
	    uint64_t new_id, int64_t price, int64_t qty, int64_t now_ns,
	    book_out_t *out);
uint32_t book_cancel_all(book_t *b, uint32_t account, book_out_t *out);
/* GTD / DAY expiry: cancel every order whose deadline <= now (DAY
 * orders expire on the transition to CLOSED). */
uint32_t book_expire(book_t *b, int64_t now_ns, book_out_t *out);
/* Session: PREOPEN -> CONTINUOUS runs the opening auction first. */
void	book_set_phase(book_t *b, book_phase_t p, book_out_t *out);

/* Uncross the book at one price (the opening / reopening auction):
 * the price that maximises executed volume, ties to the smallest
 * imbalance, then nearest the reference.  Returns the volume crossed. */
int64_t	book_auction(book_t *b, book_out_t *out);

/* Queries. */
int64_t	book_best(const book_t *b, book_side_t s);	/* BOOK_NO_PRICE */
int64_t	book_level_qty(const book_t *b, book_side_t s, int64_t price);
/* Top `depth` levels of one side, best first: price[i], qty[i]. */
uint32_t book_depth(const book_t *b, book_side_t s, uint32_t depth,
	    int64_t *price, int64_t *qty);
const book_slot_t *book_find(const book_t *b, uint64_t id);
/* Every resting order, in priority order (each side best price first,
 * FIFO within a level), to fn; stops are not included.  For reading the
 * terminal book out of the engine (tests, cross-engine comparison). */
void	book_foreach_resting(const book_t *b,
	    void (*fn)(const book_slot_t *o, void *arg), void *arg);
/* FNV-1a over the canonical book (every live order, in priority order,
 * plus the stops and the sequence): equal books have equal digests. */
uint64_t book_digest(const book_t *b);
/* Self-check of every structural invariant; 0 = sound. */
int	book_check(const book_t *b);

#endif /* L2BOOK_BOOK_H */
