/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/venue.c -- the book Isolates and the boot.
 *
 *	A book Isolate is the single writer of one symbol's book.  Its
 *	handler takes one command (a struct venue_cmd from a session fiber,
 *	through the shard's external ring), runs it through book.c, and fans
 *	the events out: execution reports to the owning session's mailbox,
 *	L2 level and trade updates to every market-data subscriber of the
 *	symbol.  It never blocks and never allocates: the book's memory is
 *	carved at boot, the event buffer is the shard's turn scratch.
 *
 *	The handler measures two stages per command, into the symbol's
 *	stats: queue (session read -> handler start: the hop across the
 *	ring) and match (the book call itself).  The session measures the
 *	round trip.  These are what /stats and the --bench report show.
 */
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "venue.h"
#include "xtc_exec.h"

/* ---- symbols ------------------------------------------------------- */

struct sym {
	char			name[VENUE_SYM_LEN];
	int			shard;
	_Atomic xtc_tnt_handle_t book;	/* set by the Isolate's init */
	struct venue_stats	stats;
	/* Market-data subscribers: written by the book Isolate only (on a
	 * VT_SUBSCRIBE), read by the same Isolate.  No lock. */
	xtc_pid_t		subs[VENUE_MAX_SUBS];
	int			nsubs;
	/* Snapshot of the top of book, published by the Isolate after each
	 * command for readers on other threads (/stats, readyz). */
	_Atomic int64_t		bid, ask, last;
	_Atomic uint64_t	seq;
};

static struct sym syms[VENUE_MAX_SYMS];
static int nsyms;
static int64_t g_ref = 10000;

int venue_nsyms(void) { return nsyms; }
const char *venue_sym_name(int i) { return syms[i].name; }
xtc_tnt_handle_t venue_sym_book(int i) { return atomic_load(&syms[i].book); }
struct venue_stats *venue_sym_stats(int i) { return &syms[i].stats; }

int
venue_sym_index(const char *name)
{
	int i;
	for (i = 0; i < nsyms; i++)
		if (strncmp(syms[i].name, name, VENUE_SYM_LEN) == 0)
			return i;
	return -1;
}

uint64_t
venue_now_ns(void)
{
	return (uint64_t)xtc_clock_mono();
}

int64_t venue_sym_best(int i, int side);
int64_t
venue_sym_best(int i, int side)
{
	return side == BOOK_BUY ? atomic_load(&syms[i].bid) :
	    atomic_load(&syms[i].ask);
}

int64_t venue_sym_last(int i);
int64_t venue_sym_last(int i) { return atomic_load(&syms[i].last); }

xtc_tnt_send_result_t
venue_to_book(int sym, uint16_t tag, const struct venue_cmd *c)
{
	xtc_tnt_handle_t h;
	xtc_tnt_send_result_t r;

	if (sym < 0 || sym >= nsyms)
		return XTC_TNT_SEND_STALE_HANDLE;
	h = atomic_load(&syms[sym].book);
	if (h == XTC_TNT_HANDLE_NONE)
		return XTC_TNT_SEND_STALE_HANDLE;
	r = xtc_tnt_send_from(XTC_TNT_HANDLE_NONE, h, tag, c, sizeof *c);
	if (r != XTC_TNT_SEND_OK)
		atomic_fetch_add(&syms[sym].stats.drops, 1);
	return r;
}

/* ---- the book Isolate ---------------------------------------------- */

/* Order ids: the venue id is (account << 40) | client_id, so a client
 * names only its own orders and two accounts never collide. */
static inline uint64_t
venue_id(uint32_t account, uint64_t client_id)
{
	return ((uint64_t)account << 40) | (client_id & ((1ull << 40) - 1));
}

static inline uint32_t
id_account(uint64_t id)
{
	return (uint32_t)(id >> 40);
}

static inline uint64_t
id_client(uint64_t id)
{
	return id & ((1ull << 40) - 1);
}

#define BOOK_EVBUF	512
#define MAX_SESSIONS	1024

typedef struct {
	int		sym;
	book_t		*b;
	void		*mem;
	/* account -> session pid that last sent a command for it (reports
	 * go there).  Account ids are small (assigned at login). */
	xtc_pid_t	owner[MAX_SESSIONS];
	book_event_t	ev[BOOK_EVBUF];
	uint64_t	commands;
} book_iso_t;

struct book_init_args {
	int sym;
};

static void
md_publish(book_iso_t *bi, const struct venue_md *m)
{
	struct sym *s = &syms[bi->sym];
	int i;

	for (i = 0; i < s->nsubs; i++)
		if (xtc_send(s->subs[i], m, sizeof *m) != XTC_OK)
			atomic_fetch_add(&s->stats.drops, 1);
}

static void
report(book_iso_t *bi, const book_event_t *e, uint64_t id, uint8_t liq,
    const struct venue_cmd *c, uint64_t t_matched)
{
	uint32_t acct = id_account(id);
	struct venue_report r;

	if (acct == 0 || acct >= MAX_SESSIONS || bi->owner[acct].gen == 0)
		return;
	memset(&r, 0, sizeof r);
	r.kind = e->kind;
	r.side = e->side;
	r.reason = e->reason;
	r.liquidity = liq;
	r.symbol = (uint16_t)bi->sym;
	r.client_id = id_client(id);
	r.other = e->kind == BOOK_EV_REPLACED ? id_client(e->other) : 0;
	r.price = e->price;
	r.qty = e->qty;
	r.seq = e->seq;
	/* Only the session that sent this command gets its timestamps. */
	r.t_ingress = (c != NULL && c->account == acct) ? c->t_ingress : 0;
	r.t_matched = t_matched;
	(void)xtc_send(bi->owner[acct], &r, sizeof r);
}

/* Fan one command's events out. */
static void
dispatch_events(book_iso_t *bi, const book_out_t *out,
    const struct venue_cmd *c, uint64_t t_matched)
{
	struct sym *s = &syms[bi->sym];
	uint32_t i;

	for (i = 0; i < out->n; i++) {
		const book_event_t *e = &out->ev[i];
		struct venue_md m;

		switch (e->kind) {
		case BOOK_EV_TRADE:
			report(bi, e, e->id, 'R', c, t_matched);
			report(bi, e, e->other, 'A', c, t_matched);
			atomic_fetch_add(&s->stats.trades, 1);
			atomic_fetch_add(&s->stats.volume, (uint64_t)e->qty);
			memset(&m, 0, sizeof m);
			m.kind = 'T';
			m.side = e->side;
			m.symbol = (uint16_t)bi->sym;
			m.price = e->price;
			m.qty = e->qty;
			m.seq = e->seq;
			md_publish(bi, &m);
			break;
		case BOOK_EV_LEVEL:
			memset(&m, 0, sizeof m);
			m.kind = 'B';
			m.side = e->side;
			m.symbol = (uint16_t)bi->sym;
			m.price = e->price;
			m.qty = e->qty;
			m.seq = e->seq;
			md_publish(bi, &m);
			break;
		case BOOK_EV_PHASE:
			memset(&m, 0, sizeof m);
			m.kind = 'H';
			m.symbol = (uint16_t)bi->sym;
			m.qty = e->qty;
			m.seq = e->seq;
			md_publish(bi, &m);
			break;
		case BOOK_EV_REJECTED:
			atomic_fetch_add(&s->stats.rejects, 1);
			report(bi, e, e->id, 0, c, t_matched);
			break;
		default:
			report(bi, e, e->id, 0, c, t_matched);
			break;
		}
	}
	atomic_store(&s->bid, book_best(bi->b, BOOK_BUY));
	atomic_store(&s->ask, book_best(bi->b, BOOK_SELL));
	if (bi->b->last_price != BOOK_NO_PRICE)
		atomic_store(&s->last, bi->b->last_price);
	atomic_store(&s->seq, bi->b->seq);
}

/* A new subscriber gets the whole book first (snapshot + every level),
 * then every change after its sequence. */
static void
send_snapshot(book_iso_t *bi, xtc_pid_t to)
{
	struct venue_md m;
	int64_t px[64], qty[64];
	uint32_t n, i;
	int side;

	memset(&m, 0, sizeof m);
	m.kind = 'S';
	m.symbol = (uint16_t)bi->sym;
	m.seq = bi->b->seq;
	m.price = bi->b->last_price;
	(void)xtc_send(to, &m, sizeof m);
	for (side = 0; side < 2; side++) {
		n = book_depth(bi->b, (book_side_t)side, 64, px, qty);
		for (i = 0; i < n; i++) {
			m.kind = 'B';
			m.side = (uint8_t)side;
			m.price = px[i];
			m.qty = qty[i];
			(void)xtc_send(to, &m, sizeof m);
		}
	}
}

static xtc_tnt_transition_t
book_init_fn(void *self, const void *args, size_t n)
{
	book_iso_t *bi = self;
	const struct book_init_args *a = args;
	book_config_t cfg;
	size_t len;

	(void)n;
	memset(bi, 0, sizeof *bi);
	bi->sym = a->sym;
	memset(&cfg, 0, sizeof cfg);
	cfg.max_orders = 1u << 17;
	cfg.tick = 1;
	cfg.ref_price = g_ref;
	cfg.band_pct_bp = 1000;		/* +-10%: a LULD-style band */
	len = book_bytes(&cfg);
	/* Boot-time allocation, once per symbol: never on the hot path. */
	if (posix_memalign(&bi->mem, 64, len) != 0 ||
	    book_init(&bi->b, bi->mem, len, &cfg) != 0)
		return xtc_tnt_transition_to_crash(XTC_TNT_FAULT_CONTRACT_VIOLATION);
	atomic_store(&syms[a->sym].bid, BOOK_NO_PRICE);
	atomic_store(&syms[a->sym].ask, BOOK_NO_PRICE);
	atomic_store(&syms[a->sym].last, g_ref);
	atomic_store(&syms[a->sym].book, xtc_tnt_self());
	xtc_tnt_register_timer(100LL * 1000 * 1000, VT_TICK);
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

static void
order_from_cmd(const struct venue_cmd *c, book_order_t *o)
{
	memset(o, 0, sizeof *o);
	o->id = venue_id(c->account, c->client_id);
	o->account = c->account;
	o->side = c->side;
	o->type = c->type;
	o->tif = c->tif;
	o->stp = c->stp;
	o->flags = c->flags;
	o->peg = c->peg;
	o->price = c->price;
	o->stop = c->stop;
	o->qty = c->qty;
	o->display = c->display;
	o->oco = c->aux ? venue_id(c->account, c->aux) : 0;
}

static xtc_tnt_transition_t
book_handler(void *self, xtc_tnt_message_t *msg)
{
	book_iso_t *bi = self;
	struct sym *s = &syms[bi->sym];
	const struct venue_cmd *c = NULL;
	book_out_t out = { bi->ev, BOOK_EVBUF, 0, 0 };
	book_order_t o;
	uint64_t t0, t1;

	if (msg->tag == VT_TICK) {
		(void)book_expire(bi->b, (int64_t)venue_now_ns(), &out);
		if (out.n)
			dispatch_events(bi, &out, NULL, venue_now_ns());
		xtc_tnt_register_timer(100LL * 1000 * 1000, VT_TICK);
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}
	if (msg->tag < VT_ORDER || msg->tag > VT_UNSUBSCRIBE)
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	c = xtc_tnt_payload_as(const struct venue_cmd, msg);
	t0 = venue_now_ns();
	if (c->t_ingress && t0 >= c->t_ingress)
		hist_record(&s->stats.queue, t0 - c->t_ingress);
	if (c->account > 0 && c->account < MAX_SESSIONS)
		bi->owner[c->account] = c->reply;

	switch (msg->tag) {
	case VT_ORDER:
		order_from_cmd(c, &o);
		book_submit(bi->b, &o, (int64_t)t0, &out);
		break;
	case VT_CANCEL:
		book_cancel(bi->b, venue_id(c->account, c->client_id),
		    c->account, &out);
		break;
	case VT_REDUCE:
		book_reduce(bi->b, venue_id(c->account, c->client_id),
		    c->account, c->qty, &out);
		break;
	case VT_REPLACE:
		book_replace(bi->b, venue_id(c->account, c->client_id),
		    c->account, venue_id(c->account, c->aux), c->price, c->qty,
		    (int64_t)t0, &out);
		break;
	case VT_CANCEL_ALL:
		(void)book_cancel_all(bi->b, c->account, &out);
		break;
	case VT_PHASE:
		book_set_phase(bi->b, (book_phase_t)c->qty, &out);
		break;
	case VT_SUBSCRIBE:
		if (s->nsubs < VENUE_MAX_SUBS) {
			s->subs[s->nsubs++] = c->reply;
			send_snapshot(bi, c->reply);
		}
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	case VT_UNSUBSCRIBE: {
		int i;
		for (i = 0; i < s->nsubs; i++)
			if (xtc_pid_eq(s->subs[i], c->reply)) {
				s->subs[i] = s->subs[--s->nsubs];
				break;
			}
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}
	}
	t1 = venue_now_ns();
	hist_record(&s->stats.match, t1 - t0);
	atomic_fetch_add(&s->stats.commands, 1);
	bi->commands++;
	dispatch_events(bi, &out, c, t1);
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

/* ---- boot ---------------------------------------------------------- */

enum { T_BOOK = 0, T_BOOT = 1 };

typedef struct {
	int x;
} boot_iso_t;

static int g_shards = 1;

static xtc_tnt_transition_t
boot_init_fn(void *self, const void *args, size_t n)
{
	int i;
	(void)self;
	(void)args;
	(void)n;
	for (i = 0; i < nsyms; i++) {
		struct book_init_args a = { i };
		(void)xtc_tnt_spawn_on((uint8_t)syms[i].shard, T_BOOK, &a,
		    sizeof a);
	}
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

static xtc_tnt_transition_t
boot_handler(void *self, xtc_tnt_message_t *msg)
{
	(void)self;
	(void)msg;
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

static uint32_t
hash_sym(const char *s)
{
	uint32_t h = 2166136261u;
	while (*s)
		h = (h ^ (uint8_t)*s++) * 16777619u;
	return h;
}

static int
parse_symbols(const char *list)
{
	const char *p = list;
	nsyms = 0;
	while (*p && nsyms < VENUE_MAX_SYMS) {
		size_t k = strcspn(p, ",");
		if (k > 0 && k < VENUE_SYM_LEN) {
			memcpy(syms[nsyms].name, p, k);
			syms[nsyms].name[k] = '\0';
			/* Symbols spread over shards by hash, then by index so
			 * a small list still uses every shard. */
			syms[nsyms].shard = (int)((hash_sym(syms[nsyms].name) +
			    (uint32_t)nsyms) % (uint32_t)g_shards);
			nsyms++;
		}
		p += k;
		if (*p == ',')
			p++;
	}
	return nsyms > 0 ? 0 : -1;
}

/* The fibers (sessions, md, web, flow) run on their own loop thread,
 * beside the shards: venue_fibers.c. */
int venue_fibers_start(const venue_opts_t *opts);
void venue_fibers_join(void);
extern _Atomic int venue_stopping;

int
venue_main(const venue_opts_t *opts)
{
	static xtc_tnt_type_t types[2];
	xtc_tnt_spec_t spec;
	int rc;

	g_shards = opts->shards;
	g_ref = opts->ref_price > 0 ? opts->ref_price : 10000;
	if (parse_symbols(opts->symbols) != 0) {
		fprintf(stderr, "l2book: no symbols\n");
		return 2;
	}
	memset(types, 0, sizeof types);
	types[T_BOOK].id = T_BOOK;
	types[T_BOOK].name = "book";
	types[T_BOOK].slot_count = VENUE_MAX_SYMS;
	types[T_BOOK].stride = sizeof(book_iso_t);
	types[T_BOOK].mailbox_capacity = 1u << 16;
	types[T_BOOK].budget_weight = VENUE_MAX_SYMS;
	types[T_BOOK].init_fn = book_init_fn;
	types[T_BOOK].handler_fn = book_handler;
	types[T_BOOT].id = T_BOOT;
	types[T_BOOT].name = "boot";
	types[T_BOOT].slot_count = 1;
	types[T_BOOT].stride = sizeof(boot_iso_t);
	types[T_BOOT].mailbox_capacity = 4;
	types[T_BOOT].budget_weight = 1;
	types[T_BOOT].init_fn = boot_init_fn;
	types[T_BOOT].handler_fn = boot_handler;
	memset(&spec, 0, sizeof spec);
	spec.name = "l2book";
	spec.types = types;
	spec.n_types = 2;
	spec.shard_count = g_shards;
	spec.scratch_size = 1u << 16;
	spec.boot_type = T_BOOT;
	/* A matching engine owns its cores: an idle shard busy-polls its
	 * rings for 50 us before it parks, so a burst never pays a wake. */
	spec.spin_ns = opts->spin_ns;

	if (venue_fibers_start(opts) != 0)
		return 1;
	rc = xtc_tnt_start(&spec);	/* blocks until xtc_tnt_stop */
	atomic_store(&venue_stopping, 1);
	venue_fibers_join();
	return rc == 0 ? 0 : 1;
}
