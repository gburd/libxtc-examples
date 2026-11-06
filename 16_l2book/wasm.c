/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/wasm.c -- the engine's exports for the browser console.
 *
 *	Compiled with book.c to freestanding wasm32 (no libc, no WASI):
 *	the page calls these to run the SAME matching engine the venue's
 *	book Isolates run, against an in-page flow.  Static memory only;
 *	the page reads results through wasm memory.
 *
 *	lob_*   one bare book (the engine on its own; the Pages check);
 *	con_*   the console's market (console.c): the engine, the noise
 *	        flow, the signals and surveillance (intrepidkarthi's
 *	        obReset / obStep / obSubmit / obSnapshot / obSignals /
 *	        obAlerts / obCancel / obSpoof / obFlood / obOpenOrders);
 *	live_*  the same signals over a book the page is given (LIVE mode:
 *	        the venue's SSE levels and trades).
 */
#include "book.h"
#include "console.h"

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

/* ---- the console market (console.c) -------------------------------- */

static unsigned char con_mem[18u << 20] __attribute__((aligned(64)));
static con_t con;
static sig_snap_t con_snap;
static con_signals_t con_sig;
static uint64_t open_id[256];
static int64_t open_side[256], open_px[256], open_q[256];

/* obReset(seed): 0, or -1 if the console's book does not fit. */
WASM_EXPORT int
con_reset(int64_t seed)
{
	if (con_book_bytes() > sizeof con_mem)
		return -1;
	return con_init(&con, con_mem, sizeof con_mem, seed);
}

/* obStep(n): the number of prints, at con_trades(). */
WASM_EXPORT uint32_t con_run(int n) { return con_step(&con, n); }
WASM_EXPORT uint32_t con_ntrades(void) { return con.n_trades; }
WASM_EXPORT con_trade_t *con_trades(void) { return con.trades; }

/* obSubmit / obCancel / obSpoof / obFlood (account 1 is "you"). */
static uint64_t con_last_id;
WASM_EXPORT int
con_order(int account, int side, int market, int64_t price, int64_t qty)
{
	return con_submit(&con, (uint32_t)account, side, market, price, qty,
	    &con_last_id);
}
WASM_EXPORT uint64_t con_order_id(void) { return con_last_id; }
WASM_EXPORT int
con_pull(int64_t id, int account)
{
	return con_cancel(&con, (uint64_t)id, (uint32_t)account);
}
WASM_EXPORT int con_do_spoof(void) { return con_spoof(&con); }
WASM_EXPORT int con_do_flood(void) { return con_flood(&con); }

/* obSnapshot(10): a sig_snap_t at the returned address. */
WASM_EXPORT sig_snap_t *
con_book(void)
{
	con_snapshot(&con, &con_snap);
	return &con_snap;
}

/* obSignals: a con_signals_t at the returned address. */
WASM_EXPORT con_signals_t *
con_sigs(void)
{
	con_sig = con_signals(&con);
	return &con_sig;
}
WASM_EXPORT uint64_t con_digest(void) { return book_digest(con.b); }
WASM_EXPORT int con_check(void) { return book_check(con.b); }
WASM_EXPORT uint64_t con_refused(void) { return con.refused; }

/* obAlerts(since): the total raised; con_alert(i) is alert i or 0. */
WASM_EXPORT uint64_t con_nalerts(void) { return con.sv.n_alerts; }
WASM_EXPORT const sv_alert_t *
con_alert(int64_t i)
{
	return sv_alert(&con.sv, (uint64_t)i);
}

/* obOpenOrders(account): the count; ids, sides, prices, sizes at the
 * con_open_* addresses. */
WASM_EXPORT uint32_t
con_open(int account)
{
	return con_open_orders(&con, (uint32_t)account, open_id, open_side,
	    open_px, open_q, 256);
}
WASM_EXPORT uint64_t *con_open_id(void) { return open_id; }
WASM_EXPORT int64_t *con_open_side(void) { return open_side; }
WASM_EXPORT int64_t *con_open_px(void) { return open_px; }
WASM_EXPORT int64_t *con_open_q(void) { return open_q; }

/* ---- LIVE: the same signals over the venue's book ------------------ */

static sig_snap_t live_in, live_pre;
static int live_have_pre;
static sig_ofi_t live_ofi;
static sig_lambda_t live_lam;
static int64_t live_cvd, live_flow;

WASM_EXPORT void
live_reset(void)
{
	__builtin_memset(&live_ofi, 0, sizeof live_ofi);
	__builtin_memset(&live_lam, 0, sizeof live_lam);
	live_have_pre = 0;
	live_cvd = live_flow = 0;
}

/* The page writes the venue's top of book here, then calls live_tick. */
WASM_EXPORT sig_snap_t *live_snap(void) { return &live_in; }

/* A print from the venue's tape: signed flow for CVD and lambda. */
WASM_EXPORT void
live_trade(int taker_sold, int64_t qty)
{
	int64_t y = taker_sold ? -qty : qty;
	live_cvd += y;
	live_flow += y;
}

/* One sampling interval: OFI over the new snapshot, and a lambda pair
 * (this interval's signed flow, its mid change) when both mids exist. */
WASM_EXPORT con_signals_t *
live_tick(void)
{
	sig_snap_t *s = &live_in;
	__builtin_memset(&con_sig, 0, sizeof con_sig);
	(void)sig_ofi_observe(&live_ofi, s);
	if (live_have_pre && live_pre.nb && live_pre.na && s->nb && s->na)
		sig_lambda_add(&live_lam, (double)live_flow,
		    ((double)s->bp[0] + (double)s->ap[0]) / 2 -
		    ((double)live_pre.bp[0] + (double)live_pre.ap[0]) / 2);
	live_pre = *s;
	live_have_pre = 1;
	live_flow = 0;
	if (s->nb && s->na) {
		con_sig.has_mid = 1;
		con_sig.mid = (s->bp[0] + s->ap[0]) / 2;
		con_sig.spread = s->ap[0] - s->bp[0];
	}
	con_sig.ofi = live_ofi.cum;
	con_sig.cvd = live_cvd;
	con_sig.imbalance = sig_depth_imbalance(s, 5);
	con_sig.best_imbalance = sig_depth_imbalance(s, 1);
	if (live_lam.n >= CON_LAMBDA_MIN)
		con_sig.lambda = sig_lambda_fit(&live_lam);
	return &con_sig;
}
