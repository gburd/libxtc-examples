/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/signals.h -- order-flow signals and market surveillance
 * for the console, in pure C (no libc, no allocation; native and wasm32).
 *
 *	The definitions are intrepidkarthi/orderbook's (pkg/signals and
 *	pkg/surveillance at 3fffc25, MIT), so the console's panels mean
 *	what theirs mean; signals.c cites each formula at its function:
 *	  OFI     best-level order-flow imbalance, Cont, Kukanov & Stoikov
 *	          (2014), accumulated across snapshots (ofi.go);
 *	  CVD     cumulative signed aggressor volume (flow.go, kyle.go);
 *	  depth   (sum bid - sum ask) / (sum bid + sum ask), top N (imbalance.go);
 *	  lambda  Kyle's price impact, OLS of mid change on signed flow
 *	          (kyle.go, regression.go);
 *	  spoof   a large order cancelled unfilled soon after placement;
 *	  OTR     placements per fill over a rolling window of events.
 *	The detectors see the event stream their Monitor sees: OrderPlaced,
 *	Trade, OrderCancelled, each with a sequence number.
 */
#ifndef L2BOOK_SIGNALS_H
#define L2BOOK_SIGNALS_H

#include <stddef.h>
#include <stdint.h>

#define SIG_DEPTH 10

/* A top-of-book view: best first, prices in ticks, sizes in lots. */
typedef struct {
	uint32_t	nb, na;
	int64_t		bp[SIG_DEPTH], bq[SIG_DEPTH];
	int64_t		ap[SIG_DEPTH], aq[SIG_DEPTH];
} sig_snap_t;

/* e_n between two snapshots (signals.OFIStep). */
int64_t	sig_ofi_step(const sig_snap_t *prev, const sig_snap_t *cur);

typedef struct {
	int		primed;
	sig_snap_t	prev;
	int64_t		cum;
} sig_ofi_t;

/* signals.OFI.Observe: the first snapshot only primes; returns e_n. */
int64_t	sig_ofi_observe(sig_ofi_t *o, const sig_snap_t *cur);

/* signals.DepthImbalance(snap, levels), in [-1, 1]; 0 for an empty book. */
double	sig_depth_imbalance(const sig_snap_t *s, uint32_t levels);

/* signals.LinReg: y = slope * x + intercept by OLS, and R^2. */
void	sig_linreg(const double *x, const double *y, uint32_t n,
	    double *slope, double *intercept, double *r2);

/* signals.EstimateLambda over a window of (signed flow, mid change). */
#define SIG_LAMBDA_WINDOW 240
typedef struct {
	double		flow[SIG_LAMBDA_WINDOW], dp[SIG_LAMBDA_WINDOW];
	uint32_t	head, n;
} sig_lambda_t;

typedef struct {
	double		lambda, intercept, r2;
	uint32_t	n;
} sig_fit_t;

void	sig_lambda_add(sig_lambda_t *l, double flow, double dp);
sig_fit_t sig_lambda_fit(const sig_lambda_t *l);

/* ---- surveillance -------------------------------------------------- */

enum { SV_PLACED = 0, SV_CANCELLED, SV_TRADE };	/* surveillance.EventKind */
enum { SV_SPOOFING = 1, SV_OTR };		/* Alert.Kind */

typedef struct {
	uint8_t		kind;
	uint32_t	user;		/* account */
	uint64_t	seq;
	uint64_t	order;		/* placed / cancelled */
	int64_t		qty;		/* placed size; trade qty */
	uint64_t	maker, taker;	/* trade */
} sv_event_t;

typedef struct {
	uint8_t		kind;		/* SV_SPOOFING, SV_OTR */
	uint32_t	user;
	uint64_t	seq;
	/* spoofing: order id, size, lifetime in events.
	 * OTR: placements, fills, window; ratio = placements / max(1, fills). */
	int64_t		a, b, c;
	double		ratio;
} sv_alert_t;

typedef struct {
	int64_t		min_size;	/* SpoofConfig.MinSize */
	uint64_t	max_lifetime;	/* SpoofConfig.MaxLifetime */
	uint64_t	otr_window;	/* OTRConfig.Window (< SV_RING) */
	uint32_t	otr_min_orders;	/* OTRConfig.MinOrders */
	double		otr_max_ratio;	/* OTRConfig.MaxRatio */
} sv_config_t;

#define SV_ORDERS	(1u << 18)	/* tracked open orders (pow2) */
#define SV_USERS	64
#define SV_RING		1024		/* per-user placement / fill seqs */
#define SV_ALERTS	1024		/* the newest alerts kept */

typedef struct {
	uint64_t	id;		/* 0 = empty */
	uint64_t	placed_seq;
	int64_t		size, filled, left;
	uint32_t	user;
	uint32_t	_pad;
} sv_order_t;

typedef struct {
	uint32_t	account;
	uint32_t	used;
	uint32_t	ph, pn, fh, fn;	/* ring heads and counts */
	uint64_t	places[SV_RING], fills[SV_RING];
} sv_user_t;

typedef struct {
	sv_config_t	cfg;
	sv_order_t	ord[SV_ORDERS];
	uint32_t	n_ord;
	uint32_t	n_users;
	sv_user_t	users[SV_USERS];
	sv_alert_t	alerts[SV_ALERTS];
	uint64_t	n_alerts;	/* total ever raised */
	uint64_t	dropped;	/* events not tracked: a table was full */
} sv_monitor_t;

void	sv_init(sv_monitor_t *m, const sv_config_t *cfg);
/* surveillance.Monitor.Observe: both detectors; returns alerts raised. */
uint32_t sv_observe(sv_monitor_t *m, const sv_event_t *e);
/* Drop the state of an order that left the market without an event the
 * detectors see (an IOC or market remainder, an STP-cancelled taker, a
 * rejection).  No later event can name it, so this changes no alert; it
 * only keeps the tables bounded by what is resting. */
void	sv_forget(sv_monitor_t *m, uint64_t order);
/* Alert number i (0-based, of n_alerts), or NULL once it has aged out. */
const sv_alert_t *sv_alert(const sv_monitor_t *m, uint64_t i);

#endif /* L2BOOK_SIGNALS_H */
