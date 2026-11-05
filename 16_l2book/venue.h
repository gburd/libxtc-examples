/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/venue.h -- the venue: book Isolates fed by session fibers.
 *
 *	One process, two concurrency models, each where it fits:
 *
 *	  ISOLATES (xtc_tnt): one stackless Isolate per symbol, on the shard
 *	  (core) the symbol hashes to.  It owns its book (book.c) and is the
 *	  only writer of it: no lock on the match path.  Its handler runs a
 *	  command to completion and returns; it never blocks.  ~100 bytes of
 *	  Isolate plus the book's working memory, dispatched in batches.
 *
 *	  FIBERS (xtc_proc): everything that blocks or holds protocol state
 *	  across reads -- the TCP order-entry sessions, the market-data
 *	  subscribers, the HTTP/SSE console, the flow generator.  Supervised
 *	  (xtc_sup), parked on their socket AND their mailbox at once
 *	  (xtc_proc_wait_fd wakes on either), written as straight-line code.
 *
 *	A fiber reaches a book with xtc_tnt_send_from (the shard's external
 *	ring); a book reaches a fiber with xtc_send (its mailbox), which the
 *	fiber's wait_fd sees.  Market data fans out through a per-symbol
 *	broadcast published by the book.
 */
#ifndef L2BOOK_VENUE_H
#define L2BOOK_VENUE_H

#include <stdint.h>

#include "book.h"
#include "hist.h"
#include "xtc.h"
#include "xtc_proc.h"
#include "xtc_tnt.h"

#define VENUE_MAX_SYMS		64
#define VENUE_MAX_SHARDS	8
#define VENUE_MAX_SUBS		64	/* market-data subscribers per symbol */
#define VENUE_SYM_LEN		8

/* Isolate message tags (xtc_tnt user tags). */
enum {
	VT_ORDER = XTC_TNT_USER_TAG_BASE + 1,	/* struct venue_cmd */
	VT_CANCEL,
	VT_REDUCE,
	VT_REPLACE,
	VT_CANCEL_ALL,
	VT_PHASE,
	VT_SUBSCRIBE,				/* a fiber wants market data */
	VT_UNSUBSCRIBE,
	VT_TICK					/* timer: GTD expiry, stats */
};

/* A command from a fiber to a book (fits the 96-byte tnt payload). */
struct venue_cmd {
	xtc_pid_t	reply;		/* the session fiber (16 bytes) */
	uint64_t	t_ingress;	/* ns: when the session read it */
	uint32_t	account;
	uint8_t		side, type, tif, stp;
	uint8_t		flags, peg;
	uint16_t	symbol;
	uint64_t	client_id;
	uint64_t	aux;		/* REPLACE: new client id; OCO partner */
	int64_t		price, stop, qty, display;
};

/* A report from a book to a session fiber (its mailbox). */
struct venue_report {
	uint8_t		kind;		/* book_ev_kind_t */
	uint8_t		side, reason, liquidity;
	uint16_t	symbol;
	uint16_t	_pad;
	uint64_t	client_id;
	uint64_t	other;		/* REPLACED: new client id */
	int64_t		price, qty;
	uint64_t	seq;
	uint64_t	t_ingress;	/* echoed: the session measures RTT */
	uint64_t	t_matched;
};

/* A market-data update from a book to a subscriber fiber. */
struct venue_md {
	uint8_t		kind;		/* 'S' snapshot hdr, 'B', 'T', 'H' */
	uint8_t		side;
	uint16_t	symbol;
	uint32_t	_pad;
	int64_t		price, qty;
	uint64_t	seq;
};

/* Per-symbol, per-stage latency (shared, read by /stats). */
struct venue_stats {
	hist_t		match;		/* book_submit/cancel wall time */
	hist_t		queue;		/* ingress -> book handler start */
	_Atomic uint64_t commands, trades, volume, rejects, drops;
};

typedef struct {
	int		shards;		/* 1..VENUE_MAX_SHARDS */
	int		fiber_loops;	/* the session fibers' exec, 1..64 */
	int		oe_port;	/* order entry, 0 = off */
	int		md_port;	/* binary market data, 0 = off */
	int		web_port;	/* HTTP + SSE console, 0 = off */
	const char	*web_root;	/* directory with index.html, book.wasm */
	const char	*symbols;	/* "AAPL,MSFT,..." */
	int		flow;		/* run the built-in flow generator */
	double		flow_rate;	/* commands/s per symbol for the flow */
	long		bench_ops;	/* > 0: run the loopback benchmark, exit */
	int		self_test;	/* run the end-to-end self-test, exit */
	int64_t		ref_price;
	int64_t		spin_ns;	/* shard busy-poll before parking */
} venue_opts_t;

int	venue_main(const venue_opts_t *opts);

/* Symbol table, fixed at start. */
int		venue_nsyms(void);
const char	*venue_sym_name(int i);
int		venue_sym_index(const char *name);
xtc_tnt_handle_t venue_sym_book(int i);	/* the book Isolate */
struct venue_stats *venue_sym_stats(int i);
int		venue_sym_shard(int i);	/* the shard (core) a symbol's book is on */

/* Session helpers (venue.c, used by the fibers). */
xtc_tnt_send_result_t venue_to_book(int sym, uint16_t tag,
	    const struct venue_cmd *c);
uint64_t venue_now_ns(void);

#endif /* L2BOOK_VENUE_H */
