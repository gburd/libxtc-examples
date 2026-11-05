/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/signals.c -- the console's order-flow signals and
 * surveillance.  See signals.h.
 *
 *	Each function carries the definition it implements and the file of
 *	intrepidkarthi/orderbook (3fffc25, MIT) it follows, so the console's
 *	panels compute what that project's console computes.  The detectors
 *	keep fixed tables instead of Go maps: open orders in an open-
 *	addressed table (backward-shift deletion, as book.c's id map),
 *	per-account rings of placement and fill sequence numbers.
 */
#include "signals.h"

#if defined(__wasm__) && !defined(__wasi__)
#define memset(d, c, n)		__builtin_memset((d), (c), (n))
#else
#include <string.h>
#endif

/* ---- OFI: Cont, Kukanov & Stoikov (2014), best level --------------- */

/*
 * e_n = dV_bid - dV_ask between consecutive snapshots, where at the best
 * level (prev (P, Q), cur (P', Q')):
 *	dV_bid =  Q'      if P' > P	dV_ask = -Q      if P' > P
 *	          Q' - Q  if P' = P	          Q' - Q  if P' = P
 *	         -Q       if P' < P	          Q'      if P' < P
 * A side missing from either snapshot contributes 0.  (pkg/signals/ofi.go
 * OFIStep, bidDelta, askDelta.)
 */
int64_t
sig_ofi_step(const sig_snap_t *prev, const sig_snap_t *cur)
{
	int64_t e = 0;

	if (prev->nb > 0 && cur->nb > 0) {
		if (cur->bp[0] > prev->bp[0])
			e += cur->bq[0];
		else if (cur->bp[0] == prev->bp[0])
			e += cur->bq[0] - prev->bq[0];
		else
			e -= prev->bq[0];
	}
	if (prev->na > 0 && cur->na > 0) {
		if (cur->ap[0] > prev->ap[0])
			e += prev->aq[0];
		else if (cur->ap[0] == prev->ap[0])
			e -= cur->aq[0] - prev->aq[0];
		else
			e -= cur->aq[0];
	}
	return e;
}

/* OFI.Observe: the first snapshot primes; later ones add e_n. */
int64_t
sig_ofi_observe(sig_ofi_t *o, const sig_snap_t *cur)
{
	int64_t e = 0;

	if (o->primed) {
		e = sig_ofi_step(&o->prev, cur);
		o->cum += e;
	}
	o->prev = *cur;
	o->primed = 1;
	return e;
}

/* ---- depth imbalance ------------------------------------------------ */

/* (sum bid qty - sum ask qty) / (sum bid qty + sum ask qty) over the top
 * `levels` of each side, in [-1, 1]; 0 for an empty book or levels 0; a
 * one-sided book is +-1.  (pkg/signals/imbalance.go DepthImbalance.) */
double
sig_depth_imbalance(const sig_snap_t *s, uint32_t levels)
{
	int64_t bid = 0, ask = 0;
	uint32_t i;

	for (i = 0; i < s->nb && i < levels; i++)
		bid += s->bq[i];
	for (i = 0; i < s->na && i < levels; i++)
		ask += s->aq[i];
	if (bid + ask == 0)
		return 0;
	return (double)(bid - ask) / (double)(bid + ask);
}

/* ---- Kyle's lambda -------------------------------------------------- */

/*
 * OLS fit y = slope * x + intercept, R^2 = sxy^2 / (sxx * syy) (the
 * square of Pearson's r).  Zeros for n < 2; (0, mean y, 0) when x has no
 * variance; R^2 0 when y is constant.  The sums run in index order, as
 * pkg/signals/regression.go LinReg's do, so the floats come out the same.
 */
void
sig_linreg(const double *x, const double *y, uint32_t n, double *slope,
    double *intercept, double *r2)
{
	double sx = 0, sy = 0, mx, my, sxx = 0, sxy = 0, syy = 0;
	uint32_t i;

	*slope = *intercept = *r2 = 0;
	if (n < 2)
		return;
	for (i = 0; i < n; i++) {
		sx += x[i];
		sy += y[i];
	}
	mx = sx / (double)n;
	my = sy / (double)n;
	for (i = 0; i < n; i++) {
		double dx = x[i] - mx, dy = y[i] - my;
		sxx += dx * dx;
		sxy += dx * dy;
		syy += dy * dy;
	}
	if (sxx == 0) {
		*intercept = my;
		return;
	}
	*slope = sxy / sxx;
	*intercept = my - *slope * mx;
	if (syy != 0)
		*r2 = (sxy * sxy) / (sxx * syy);
}

/* A rolling window of (signed flow, mid change) pairs; the oldest drops
 * out first (the console's lambdaWindow). */
void
sig_lambda_add(sig_lambda_t *l, double flow, double dp)
{
	uint32_t at = (l->head + l->n) % SIG_LAMBDA_WINDOW;

	l->flow[at] = flow;
	l->dp[at] = dp;
	if (l->n < SIG_LAMBDA_WINDOW)
		l->n++;
	else
		l->head = (l->head + 1) % SIG_LAMBDA_WINDOW;
}

/* dP = lambda * y + c by OLS over the window, oldest first.  (pkg/
 * signals/kyle.go EstimateLambda.) */
sig_fit_t
sig_lambda_fit(const sig_lambda_t *l)
{
	static double x[SIG_LAMBDA_WINDOW], y[SIG_LAMBDA_WINDOW];
	sig_fit_t f;
	uint32_t i;

	memset(&f, 0, sizeof f);
	if (l->n < 2)
		return f;
	for (i = 0; i < l->n; i++) {
		x[i] = l->flow[(l->head + i) % SIG_LAMBDA_WINDOW];
		y[i] = l->dp[(l->head + i) % SIG_LAMBDA_WINDOW];
	}
	sig_linreg(x, y, l->n, &f.lambda, &f.intercept, &f.r2);
	f.n = l->n;
	return f;
}

/* ---- surveillance: the open-order table ----------------------------- */

#define SV_NONE 0xffffffffu

static uint32_t
sv_hash(uint64_t id)
{
	id ^= id >> 33;
	id *= 0xff51afd7ed558ccdull;
	id ^= id >> 33;
	return (uint32_t)id & (SV_ORDERS - 1);
}

static uint32_t
ord_find(const sv_monitor_t *m, uint64_t id)
{
	uint32_t h = sv_hash(id);

	while (m->ord[h].id != 0) {
		if (m->ord[h].id == id)
			return h;
		h = (h + 1) & (SV_ORDERS - 1);
	}
	return SV_NONE;
}

static sv_order_t *
ord_put(sv_monitor_t *m, uint64_t id)
{
	uint32_t h = sv_hash(id);

	if (m->n_ord + 1 > SV_ORDERS / 4 * 3)
		return NULL;
	while (m->ord[h].id != 0 && m->ord[h].id != id)
		h = (h + 1) & (SV_ORDERS - 1);
	if (m->ord[h].id == 0)
		m->n_ord++;
	m->ord[h].id = id;
	return &m->ord[h];
}

static void
ord_del(sv_monitor_t *m, uint32_t h)
{
	const uint32_t mask = SV_ORDERS - 1;
	uint32_t j;

	for (j = (h + 1) & mask; m->ord[j].id != 0; j = (j + 1) & mask) {
		uint32_t home = sv_hash(m->ord[j].id);
		if (((j - home) & mask) >= ((j - h) & mask)) {
			m->ord[h] = m->ord[j];
			h = j;
		}
	}
	memset(&m->ord[h], 0, sizeof m->ord[h]);
	m->n_ord--;
}

static sv_user_t *
user_get(sv_monitor_t *m, uint32_t account)
{
	uint32_t i;

	for (i = 0; i < m->n_users; i++)
		if (m->users[i].account == account)
			return &m->users[i];
	if (m->n_users == SV_USERS)
		return NULL;
	m->users[m->n_users].account = account;
	return &m->users[m->n_users++];
}

/* Append a sequence number to a ring (oldest dropped when full: with
 * otr_window < SV_RING - 1 the dropped one is outside every window). */
static void
ring_push(uint64_t *r, uint32_t *head, uint32_t *n, uint64_t seq)
{
	r[(*head + *n) % SV_RING] = seq;
	if (*n < SV_RING)
		(*n)++;
	else
		*head = (*head + 1) % SV_RING;
}

/* pruneSeqs: keep only seq >= cutoff (the ring is in seq order). */
static void
ring_prune(const uint64_t *r, uint32_t *head, uint32_t *n, uint64_t cutoff)
{
	while (*n > 0 && r[*head] < cutoff) {
		*head = (*head + 1) % SV_RING;
		(*n)--;
	}
}

static void
alert(sv_monitor_t *m, const sv_alert_t *a)
{
	m->alerts[m->n_alerts % SV_ALERTS] = *a;
	m->n_alerts++;
}

void
sv_init(sv_monitor_t *m, const sv_config_t *cfg)
{
	memset(m, 0, sizeof *m);
	m->cfg = *cfg;
	if (m->cfg.otr_window >= SV_RING - 1)
		m->cfg.otr_window = SV_RING - 2;
}

/*
 * OTRDetector.rate: prune the account's placements and fills to
 * [seq - Window, seq]; with at least MinOrders placements in the window,
 * flag placements / max(1, fills) > MaxRatio.
 */
static uint32_t
otr_rate(sv_monitor_t *m, sv_user_t *u, uint64_t seq)
{
	uint64_t cutoff = seq > m->cfg.otr_window ? seq - m->cfg.otr_window : 0;
	uint32_t denom;
	double ratio;
	sv_alert_t a;

	ring_prune(u->places, &u->ph, &u->pn, cutoff);
	ring_prune(u->fills, &u->fh, &u->fn, cutoff);
	if (u->pn < m->cfg.otr_min_orders)
		return 0;
	denom = u->fn ? u->fn : 1;
	ratio = (double)u->pn / (double)denom;
	if (!(ratio > m->cfg.otr_max_ratio))
		return 0;
	memset(&a, 0, sizeof a);
	a.kind = SV_OTR;
	a.user = u->account;
	a.seq = seq;
	a.a = u->pn;
	a.b = u->fn;
	a.c = (int64_t)m->cfg.otr_window;
	a.ratio = ratio;
	alert(m, &a);
	return 1;
}

/* A print against a tracked order: count the fill for its owner (OTR),
 * mark it filled (spoof), and forget it once nothing is left
 * (openSizes.fill). */
static void
sv_fill(sv_monitor_t *m, uint64_t id, int64_t qty, uint64_t seq)
{
	uint32_t h = id ? ord_find(m, id) : SV_NONE;
	sv_order_t *o;
	sv_user_t *u;

	if (h == SV_NONE)
		return;
	o = &m->ord[h];
	u = user_get(m, o->user);
	if (u != NULL)
		ring_push(u->fills, &u->fh, &u->fn, seq);
	else
		m->dropped++;
	o->filled += qty;
	o->left -= qty;
	if (o->left <= 0)
		ord_del(m, h);
}

uint32_t
sv_observe(sv_monitor_t *m, const sv_event_t *e)
{
	sv_order_t *o;
	sv_user_t *u;
	uint32_t h;
	uint64_t lifetime;
	sv_alert_t a;

	switch (e->kind) {
	case SV_PLACED:
		/* SpoofDetector and OTRDetector both record the order. */
		if (e->qty > 0) {
			o = ord_put(m, e->order);
			if (o != NULL) {
				o->user = e->user;
				o->placed_seq = e->seq;
				o->size = e->qty;
				o->filled = 0;
				o->left = e->qty;
			} else
				m->dropped++;
		}
		u = user_get(m, e->user);
		if (u == NULL) {
			m->dropped++;
			return 0;
		}
		ring_push(u->places, &u->ph, &u->pn, e->seq);
		return otr_rate(m, u, e->seq);
	case SV_TRADE:
		sv_fill(m, e->maker, e->qty, e->seq);
		if (e->taker != e->maker)
			sv_fill(m, e->taker, e->qty, e->seq);
		return 0;
	case SV_CANCELLED:
		/*
		 * SpoofDetector: an order of at least MinSize cancelled with
		 * nothing filled within MaxLifetime events of its placement.
		 */
		h = ord_find(m, e->order);
		if (h == SV_NONE)
			return 0;
		o = &m->ord[h];
		lifetime = e->seq - o->placed_seq;
		memset(&a, 0, sizeof a);
		a.kind = SV_SPOOFING;
		a.user = o->user;
		a.seq = e->seq;
		a.a = (int64_t)o->id;
		a.b = o->size;
		a.c = (int64_t)lifetime;
		h = o->filled == 0 && o->size >= m->cfg.min_size &&
		    lifetime <= m->cfg.max_lifetime;
		ord_del(m, ord_find(m, e->order));
		if (h)
			alert(m, &a);
		return h;
	}
	return 0;
}

void
sv_forget(sv_monitor_t *m, uint64_t order)
{
	uint32_t h = ord_find(m, order);

	if (h != SV_NONE)
		ord_del(m, h);
}

const sv_alert_t *
sv_alert(const sv_monitor_t *m, uint64_t i)
{
	if (i >= m->n_alerts || m->n_alerts - i > SV_ALERTS)
		return NULL;
	return &m->alerts[i % SV_ALERTS];
}
