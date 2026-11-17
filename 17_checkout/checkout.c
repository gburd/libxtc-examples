/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/17_checkout/checkout.c -- an order checkout service: one saga
 * per order, backend connections from a pool, and the request context in
 * the per-process dictionary.
 *
 * Each order runs in its own xtc_proc.  Checkout touches systems that
 * cannot share a transaction (inventory, payments, shipping), so the
 * order is a saga (xtc_saga): quote, reserve stock, charge the card, book
 * the shipment.  When a step fails, the steps that already succeeded are
 * undone in reverse order (refund the card, release the stock) and the
 * order reports which step failed and why.
 *
 * Every step makes a round trip to the backend over a connection, and the
 * backend accepts only POOL_CONNS connections, fewer than the orders in
 * flight.  An xtc_pool hands them out: an order parks in
 * xtc_pool_checkout until a connection frees, so the pool is also the
 * limit on concurrent checkouts.
 *
 * The connection and the request (its id, trace id and trace log) live in
 * the order proc's dictionary (xtc_pdict), put there once when the request
 * starts.  The backend client and the trace helper read them from there
 * instead of having them passed through every saga step, the way a
 * threaded server keeps a request id in thread-local storage.  Thread-local
 * storage is wrong here: every order shares the loop's one OS thread.
 * The dictionary is per proc.
 *
 * Output is deterministic: each order writes its trace into its own
 * buffer, and main prints the orders in id order after the run, then
 * checks the ledger (stock, balances, shipments, the pool) against what
 * the outcomes imply.  Exit 0 iff every check holds.
 *
 * Uses only the public xtc_* API (this is a consumer program).
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_pdict.h"
#include "xtc_pool.h"
#include "xtc_proc.h"
#include "xtc_saga.h"

#define POOL_CONNS 2                           /* the backend's limit */
#define ROUNDTRIP_NS (1000LL * 1000)           /* 1 ms per backend call */
#define CHECKOUT_TIMEOUT_NS (5000LL * 1000000) /* give up after 5 s */
#define LOG_CAP 1024

enum sku { SKU_WIDGET, SKU_GADGET, SKU_GIZMO, N_SKU };

static const char *const sku_name[N_SKU] = { "widget", "gadget", "gizmo" };
static const int sku_price[N_SKU] = { 1250, 4999, 89900 }; /* cents */
static const int sku_stock0[N_SKU] = { 10, 4, 2 };

enum customer { ALICE, BOB, CAROL, DAVE, ERIN, FRANK, N_CUSTOMER };

static const char *const cust_name[N_CUSTOMER] = { "alice", "bob", "carol",
	"dave", "erin", "frank" };
static const int cust_balance0[N_CUSTOMER] = { 100000, 20000, 50000, 30000,
	5000, 100000 };

/* Regions the shipping service has a carrier for. */
static const char *const ship_regions[] = { "Boston", "Lisbon", "Osaka",
	"Nairobi", "Toronto", NULL };

/* The saga's steps, in order.  Index = the value xtc_saga_failed_step()
 * reports. */
enum { STEP_QUOTE, STEP_RESERVE, STEP_CHARGE, STEP_SHIP, N_STEPS };

static const char *const step_name[N_STEPS] = { "quote", "reserve", "charge",
	"ship" };

/*
 * The systems of record.  Every order proc runs on the one loop, and a
 * proc runs until it parks, so a check-then-update with no park between
 * them needs no lock.
 */
struct backend {
	int stock[N_SKU];
	int balance[N_CUSTOMER];
	int charges;   /* card charges made */
	int refunds;   /* charges refunded */
	int shipments; /* shipments booked */
};

/* One backend connection.  The pool hands out pointers to these. */
struct conn {
	int calls; /* round trips made on this connection */
};

/*
 * One order: the input, the state the saga steps build up, and the
 * outcome.  The order is both the saga steps' ctx and the request
 * context in the order proc's dictionary.
 */
struct order {
	int id;
	enum customer cust;
	enum sku sku;
	int qty;
	const char *ship_to;
	int expect_ok;   /* the scenario's expected outcome */
	int expect_step; /* expected failing step, or -1 */

	char trace_id[9];
	int amount;      /* cents, set by quote */
	const char *why; /* the failing step's reason */

	int rc; /* xtc_saga_run's result */
	int failed_step;
	int completed;
	int pdict_left; /* entries left after the clear */
	char log[LOG_CAP];
	size_t loglen;
};

#define ORDER(i, c, k, q, to, ok, step)                                        \
	{ .id = (i),                                                           \
		.cust = (c),                                                   \
		.sku = (k),                                                    \
		.qty = (q),                                                    \
		.ship_to = (to),                                               \
		.expect_ok = (ok),                                             \
		.expect_step = (step) }

/* The scenario: three orders go through, three fail at different steps,
 * so each failure unwinds a different prefix. */
static struct order orders[] = {
	ORDER(1001, ALICE, SKU_WIDGET, 2, "Boston", 1, -1),
	ORDER(1002, BOB, SKU_GADGET, 1, "Lisbon", 1, -1),
	ORDER(1003, CAROL, SKU_GIZMO, 1, "Osaka", 0, STEP_CHARGE),
	ORDER(1004, DAVE, SKU_WIDGET, 3, "Antarctica", 0, STEP_SHIP),
	ORDER(1005, ERIN, SKU_WIDGET, 1, "Nairobi", 1, -1),
	ORDER(1006, FRANK, SKU_GADGET, 5, "Toronto", 0, STEP_RESERVE),
};
#define N_ORDERS ((int)(sizeof orders / sizeof orders[0]))

static struct backend be;
static struct conn conns[POOL_CONNS];
static xtc_pool_t *pool;

/* Pool use, as the order procs see it. */
static int in_use, peak_in_use, queued;

/* ---- request context (the per-process dictionary) ---------------- */

/* The calling order proc's request.  Put by order_proc before the saga
 * runs, so every layer below may rely on it. */
static struct order *
cur_req(void)
{
	void *v = NULL;

	(void)xtc_pdict_get("req", &v);
	return v;
}

/* The connection the calling order proc holds. */
static struct conn *
cur_conn(void)
{
	void *v = NULL;

	(void)xtc_pdict_get("conn", &v);
	return v;
}

/* Append one line to the current request's trace log, prefixed with its
 * trace id.  A line that does not fit is cut short. */
static void
trace(const char *fmt, ...)
{
	struct order *o = cur_req();
	va_list ap;
	int n;

	if (o == NULL || o->loglen >= sizeof o->log)
		return;
	n = snprintf(o->log + o->loglen, sizeof o->log - o->loglen, "  [%s] ",
	    o->trace_id);
	if (n > 0)
		o->loglen += (size_t)n;
	if (o->loglen >= sizeof o->log) {
		o->loglen = sizeof o->log - 1;
		return;
	}
	va_start(ap, fmt);
	n = vsnprintf(o->log + o->loglen, sizeof o->log - o->loglen, fmt, ap);
	va_end(ap);
	if (n > 0)
		o->loglen += (size_t)n;
	if (o->loglen >= sizeof o->log - 1) {
		o->loglen = sizeof o->log - 1;
		return;
	}
	o->log[o->loglen++] = '\n';
	o->log[o->loglen] = '\0';
}

/* One backend round trip on the current connection.  The proc parks for
 * the network latency, so other orders run meanwhile. */
static void
db_roundtrip(void)
{
	struct conn *c = cur_conn();

	if (c != NULL)
		c->calls++;
	(void)xtc_proc_sleep(ROUNDTRIP_NS);
}

/* Format `cents` as dollars into buf. */
static const char *
money(char *buf, size_t len, int cents)
{
	(void)snprintf(buf, len, "$%d.%02d", cents / 100, cents % 100);
	return buf;
}

/* ---- the saga steps ----------------------------------------------- */

/* Price the order.  A read: nothing to undo, so it has no compensation. */
static int
step_quote(void *arg)
{
	struct order *o = arg;
	char m[16];

	db_roundtrip();
	o->amount = sku_price[o->sku] * o->qty;
	trace("quote: %d x %s = %s", o->qty, sku_name[o->sku],
	    money(m, sizeof m, o->amount));
	return XTC_OK;
}

/* Take the items out of stock, or fail if there are not enough. */
static int
step_reserve(void *arg)
{
	struct order *o = arg;

	db_roundtrip();
	if (be.stock[o->sku] < o->qty) {
		o->why = "out of stock";
		trace("reserve %d x %s: out of stock", o->qty,
		    sku_name[o->sku]);
		return XTC_E_RESOURCE;
	}
	be.stock[o->sku] -= o->qty;
	trace("reserve %d x %s: ok", o->qty, sku_name[o->sku]);
	return XTC_OK;
}

/* Compensation for step_reserve: put the items back. */
static int
undo_reserve(void *arg)
{
	struct order *o = arg;

	db_roundtrip();
	be.stock[o->sku] += o->qty;
	trace("release %d x %s", o->qty, sku_name[o->sku]);
	return XTC_OK;
}

/* Charge the customer's card, or fail if the card is declined. */
static int
step_charge(void *arg)
{
	struct order *o = arg;
	char m[16];

	db_roundtrip();
	if (be.balance[o->cust] < o->amount) {
		o->why = "card declined";
		trace("charge %s %s: declined", cust_name[o->cust],
		    money(m, sizeof m, o->amount));
		return XTC_E_ABORTED;
	}
	be.balance[o->cust] -= o->amount;
	be.charges++;
	trace("charge %s %s: ok", cust_name[o->cust],
	    money(m, sizeof m, o->amount));
	return XTC_OK;
}

/* Compensation for step_charge: refund the charge. */
static int
undo_charge(void *arg)
{
	struct order *o = arg;
	char m[16];

	db_roundtrip();
	be.balance[o->cust] += o->amount;
	be.refunds++;
	trace("refund %s %s", cust_name[o->cust],
	    money(m, sizeof m, o->amount));
	return XTC_OK;
}

/*
 * Book the shipment, or fail if no carrier serves the destination.  This
 * is the last step, so it needs no compensation: a compensation runs only
 * when a LATER step fails, and nothing comes after it.
 */
static int
step_ship(void *arg)
{
	struct order *o = arg;
	int i;

	db_roundtrip();
	for (i = 0; ship_regions[i] != NULL; i++) {
		if (strcmp(ship_regions[i], o->ship_to) == 0) {
			be.shipments++;
			trace("ship to %s: booked", o->ship_to);
			return XTC_OK;
		}
	}
	o->why = "no carrier";
	trace("ship to %s: no carrier serves it", o->ship_to);
	return XTC_E_INVAL;
}

/* ---- the order proc ----------------------------------------------- */

/* Build the checkout saga for `o`: four steps, two with an undo. */
static int
build_saga(struct order *o, xtc_saga_t **out)
{
	xtc_saga_t *s = NULL;
	int rc;

	if ((rc = xtc_saga_create(&s)) != XTC_OK)
		return rc;
	if ((rc = xtc_saga_step(s, step_quote, NULL, o)) != XTC_OK ||
	    (rc = xtc_saga_step(s, step_reserve, undo_reserve, o)) != XTC_OK ||
	    (rc = xtc_saga_step(s, step_charge, undo_charge, o)) != XTC_OK ||
	    (rc = xtc_saga_step(s, step_ship, NULL, o)) != XTC_OK) {
		xtc_saga_destroy(s);
		return rc;
	}
	*out = s;
	return XTC_OK;
}

/* Take a connection, waiting for one when all are busy.  Counts the
 * requests that had to queue. */
static int
conn_checkout(struct conn **out)
{
	void *res = NULL;
	int rc;

	rc = xtc_pool_checkout(pool, 0, &res);
	if (rc == XTC_E_AGAIN) {
		queued++;
		rc = xtc_pool_checkout(pool, CHECKOUT_TIMEOUT_NS, &res);
	}
	if (rc != XTC_OK)
		return rc;
	if (++in_use > peak_in_use)
		peak_in_use = in_use;
	*out = res;
	return XTC_OK;
}

/*
 * One order, start to finish: set up the request context, take a
 * connection, run the saga, give the connection back.  The outcome is
 * left in the order for main to report.
 */
static void
order_proc(void *arg)
{
	struct order *o = arg;
	struct conn *c = NULL;
	xtc_saga_t *saga = NULL;
	int rc;

	o->failed_step = -1;
	(void)snprintf(o->trace_id, sizeof o->trace_id, "%08x",
	    (unsigned)o->id * 2654435761u);
	if ((rc = xtc_pdict_put("req", o)) != XTC_OK)
		goto out;
	if ((rc = conn_checkout(&c)) != XTC_OK) {
		o->why = "no backend connection";
		goto out;
	}
	if ((rc = xtc_pdict_put("conn", c)) != XTC_OK ||
	    (rc = build_saga(o, &saga)) != XTC_OK)
		goto out;

	rc = xtc_saga_run(saga);
	o->failed_step = xtc_saga_failed_step(saga);
	o->completed = xtc_saga_n_completed(saga);

out:
	o->rc = rc;
	xtc_saga_destroy(saga);
	if (c != NULL) {
		in_use--;
		(void)xtc_pool_checkin(pool, c);
	}
	/* The request is over: drop its context before the proc ends. */
	(void)xtc_pdict_clear();
	o->pdict_left = xtc_pdict_count();
}

/* ---- the report and the checks ------------------------------------ */

static int failures;

static void
check(int ok, const char *what)
{
	if (!ok) {
		printf("CHECK FAILED: %s\n", what);
		failures++;
	}
}

/* Print one order's outcome and trace, and check it against the
 * scenario. */
static void
report_order(const struct order *o)
{
	printf("order %d %s: %d x %s to %s: ", o->id, cust_name[o->cust],
	    o->qty, sku_name[o->sku], o->ship_to);
	if (o->rc == XTC_OK)
		printf("OK\n");
	else if (o->failed_step >= 0)
		printf("FAILED at %s (%s) after %d completed step%s\n",
		    step_name[o->failed_step], o->why, o->completed,
		    o->completed == 1 ? "" : "s");
	else
		printf("FAILED (%s)\n", xtc_strerror(o->rc));
	fputs(o->log, stdout);

	check((o->rc == XTC_OK) == o->expect_ok, "order outcome");
	check(o->failed_step == o->expect_step, "failing step");
	check(o->pdict_left == 0, "request context cleared");
}

/* Check the systems of record against the expected outcomes: every
 * successful order took its stock and its money and booked a shipment,
 * and every failed one left no trace. */
static void
check_ledger(void)
{
	int stock[N_SKU], balance[N_CUSTOMER];
	int i, ok_orders = 0, refunds = 0, calls = 0;

	memcpy(stock, sku_stock0, sizeof stock);
	memcpy(balance, cust_balance0, sizeof balance);
	for (i = 0; i < N_ORDERS; i++) {
		const struct order *o = &orders[i];
		if (o->expect_ok) {
			ok_orders++;
			stock[o->sku] -= o->qty;
			balance[o->cust] -= sku_price[o->sku] * o->qty;
		} else if (o->expect_step > STEP_CHARGE) {
			refunds++;
		}
	}
	for (i = 0; i < POOL_CONNS; i++)
		calls += conns[i].calls;

	printf("ledger: stock widget=%d gadget=%d gizmo=%d; %d shipments; "
	       "%d charges, %d refunds\n",
	    be.stock[SKU_WIDGET], be.stock[SKU_GADGET], be.stock[SKU_GIZMO],
	    be.shipments, be.charges, be.refunds);
	printf(
	    "pool: %d connections, peak %d in use, %d orders queued for one, "
	    "%d round trips\n",
	    POOL_CONNS, peak_in_use, queued, calls);

	check(memcmp(stock, be.stock, sizeof stock) == 0, "stock");
	check(memcmp(balance, be.balance, sizeof balance) == 0, "balances");
	check(be.shipments == ok_orders, "shipments");
	check(be.charges == ok_orders + refunds, "charges");
	check(be.refunds == refunds, "refunds");
	check(peak_in_use == POOL_CONNS, "pool limit reached, not exceeded");
	check(queued > 0, "some orders queued for a connection");
	check(xtc_pool_available(pool) == POOL_CONNS, "every connection back");
}

int
main(int argc, char **argv)
{
	xtc_proc_opts_t po = XTC_PROC_OPTS_INIT;
	xtc_loop_t *loop = NULL;
	int i;

	if (argc > 1) {
		printf("usage: %s\n"
		       "Runs %d checkouts as sagas over %d pooled backend "
		       "connections, two of them failing part way; prints each "
		       "order's trace and exits 0 iff the ledger balances.\n",
		    argv[0], N_ORDERS, POOL_CONNS);
		return strcmp(argv[1], "--help") == 0 ? 0 : 2;
	}

	memcpy(be.stock, sku_stock0, sizeof be.stock);
	memcpy(be.balance, cust_balance0, sizeof be.balance);
	if (xtc_loop_create(&loop) != XTC_OK ||
	    xtc_pool_create(POOL_CONNS, &pool) != XTC_OK) {
		fprintf(stderr, "checkout: loop or pool init failed\n");
		return 1;
	}
	for (i = 0; i < POOL_CONNS; i++) {
		if (xtc_pool_add(pool, &conns[i]) != XTC_OK) {
			fprintf(stderr, "checkout: pool add failed\n");
			return 1;
		}
	}
	po.name = "order";
	for (i = 0; i < N_ORDERS; i++) {
		if (xtc_proc_spawn(loop, order_proc, &orders[i], &po, NULL) !=
		    XTC_OK) {
			fprintf(stderr, "checkout: spawn failed\n");
			return 1;
		}
	}
	(void)xtc_loop_run(loop);
	xtc_loop_destroy(loop);

	printf("checkout: %d orders, %d backend connections\n", N_ORDERS,
	    POOL_CONNS);
	for (i = 0; i < N_ORDERS; i++)
		report_order(&orders[i]);
	check_ledger();
	xtc_pool_destroy(pool);

	if (failures != 0) {
		printf("FAIL: %d check(s) failed\n", failures);
		return 1;
	}
	printf("OK\n");
	return 0;
}
