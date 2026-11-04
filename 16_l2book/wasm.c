/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/wasm.c -- the engine's exports for the browser console.
 *
 *	Compiled with book.c to freestanding wasm32 (no libc, no WASI):
 *	the page calls these to run the SAME matching engine the venue's
 *	book Isolates run, against an in-page flow.  One static book, a
 *	static event buffer the page reads through wasm memory.
 */
#include "book.h"

#define WASM_EXPORT __attribute__((visibility("default")))
#define MEM_BYTES (24u << 20)

static unsigned char mem[MEM_BYTES] __attribute__((aligned(64)));
static book_t *b;
static book_event_t ev[4096];
static book_out_t out;
static int64_t depth_px[64], depth_q[64];

WASM_EXPORT int
lob_init(int64_t ref_price, int max_orders)
{
	book_config_t cfg;
	__builtin_memset(&cfg, 0, sizeof cfg);
	cfg.max_orders = (uint32_t)max_orders;
	cfg.tick = 1;
	cfg.ref_price = ref_price;
	cfg.band_pct_bp = 1000;
	if (book_bytes(&cfg) > MEM_BYTES)
		return -1;
	out.ev = ev;
	out.cap = sizeof ev / sizeof ev[0];
	return book_init(&b, mem, MEM_BYTES, &cfg);
}

/* The events of the last command: count, and a pointer for the page. */
WASM_EXPORT uint32_t lob_ev_count(void) { return out.n; }
WASM_EXPORT book_event_t *lob_ev(void) { return ev; }
WASM_EXPORT int lob_ev_size(void) { return (int)sizeof(book_event_t); }

WASM_EXPORT void
lob_submit(int64_t id, int account, int side, int type, int tif, int flags,
    int64_t price, int64_t stop, int64_t qty, int64_t display)
{
	book_order_t o;
	__builtin_memset(&o, 0, sizeof o);
	o.id = (uint64_t)id;
	o.account = (uint32_t)account;
	o.side = (uint8_t)side;
	o.type = (uint8_t)type;
	o.tif = (uint8_t)tif;
	o.flags = (uint8_t)flags;
	o.price = price;
	o.stop = stop;
	o.qty = qty;
	o.display = display;
	out.n = 0;
	book_submit(b, &o, 0, &out);
}

WASM_EXPORT void
lob_cancel(int64_t id, int account)
{
	out.n = 0;
	book_cancel(b, (uint64_t)id, (uint32_t)account, &out);
}

WASM_EXPORT int64_t lob_best(int side) { return book_best(b, (book_side_t)side); }

/* Top n levels of a side into the depth arrays; returns the count. */
WASM_EXPORT uint32_t
lob_depth(int side, uint32_t n)
{
	return book_depth(b, (book_side_t)side, n > 64 ? 64 : n, depth_px,
	    depth_q);
}
WASM_EXPORT int64_t *lob_depth_px(void) { return depth_px; }
WASM_EXPORT int64_t *lob_depth_q(void) { return depth_q; }

WASM_EXPORT uint64_t lob_digest(void) { return book_digest(b); }
WASM_EXPORT uint64_t lob_seq(void) { return b->seq; }
WASM_EXPORT uint64_t lob_trades(void) { return b->trades; }
WASM_EXPORT int64_t lob_last(void) { return b->last_price; }
WASM_EXPORT int lob_check(void) { return book_check(b); }
