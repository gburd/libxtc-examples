/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/19_edgeacl/edgeacl.c -- an edge gateway's IP access table: an
 * ordered map of address ranges read by sessions on every loop while a
 * feed rewrites it, and run-time settings with per-session overrides.
 *
 * THE TABLE.  The gateway decides what to do with a client by its
 * address: which allocation it falls in, which country that is, and
 * whether the range is allowed or denied.  The table is a set of
 * disjoint ranges [start, end], and the question is "which range holds
 * this address": the range with the largest start <= the address, if
 * its end reaches the address.  That is a floor lookup, which a hash
 * table cannot answer and xtc_cskip can (xtc_cskip_floor).  A feed
 * process rewrites the table while the sessions read it: it reassigns
 * ranges (a replace), adds ranges in gaps, withdraws ranges, and splits
 * one range into two (a remove and two inserts).
 *
 * Readers take no lock of their own: a lookup is an xtc_rcu read-side
 * around xtc_cskip_floor, and the answer is copied out before the
 * read-side ends.  A removed node, and the range record the feed
 * replaced or removed, are retired through xtc_rcu_retire and freed only
 * after every reader that could still see them has left its read-side.
 * The feed calls xtc_rcu_synchronize every few updates to reclaim them.
 *
 * THE SETTINGS.  Two settings live in xtc_cfg: edge.mode (enforce, or
 * audit: log what would be denied and let it through) and edge.unknown
 * (what to do with an address no range holds).  Each session binds its
 * own xtc_cfg session, so a read resolves "this session's override,
 * else the global value".  The two partner sessions set edge.mode=audit
 * from their connection's startup options (source CLIENT); the public
 * sessions have no override.  Halfway through, the operator changes the
 * global edge.unknown from allow to deny, and every session without an
 * override sees it on its next read, on every loop.  A request with a
 * dry-run flag pushes a level, sets audit for that one request and
 * aborts the level (SET LOCAL).  The hot path reads the settings through
 * xtc_cfg_ref handles, resolved once.
 *
 * CHECKING IT.  Each feed update has a version.  The program keeps a
 * snapshot of the table after every version, and each live lookup is
 * checked against the snapshots: the answer must be the table's answer
 * at some version between the one completed before the lookup began and
 * the one started before it ended.  Then the table and the settings are
 * frozen between barriers, and every session answers the same scripted
 * requests; their answers are printed and checked: the public sessions
 * agree with each other, the partner sessions agree with each other, and
 * each matches the snapshot of the final table.  The printed output is
 * deterministic; how many lookups the sessions made during the feed is
 * not, and is printed only with -v.
 *
 * Uses only the public xtc_* API (this is a consumer program).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_cfg.h"
#include "xtc_cskip.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_rcu.h"
#include "xtc_sync.h"

#define N_LOOPS 4
#define N_SESSIONS 8
#define N_PARTNER 2 /* the last two sessions */
#define N_BLOCKS 48 /* the address plan: 10.0/16 .. 10.47/16 */
#define N_FEED 48   /* at most this many feed updates */
#define MAX_RANGES 128
#define FEED_POLL_NS (50LL * 1000) /* the feed's wait between checks */
#define OVERLAP 64                 /* lookups per session per update */
#define RECLAIM_EVERY 8            /* xtc_rcu_synchronize cadence */
#define BATCH 32                   /* lookups between yields */

enum action { ALLOW, DENY };
enum mode { MODE_ENFORCE, MODE_AUDIT };
enum verdict { V_ALLOW, V_DENY, V_AUDIT };

static const char *const mode_labels[] = { "enforce", "audit", NULL };
static const char *const unknown_labels[] = { "allow", "deny", NULL };
static const char *const verdict_name[] = { "allow", "deny", "audit" };
static const char *const countries[] = { "US", "DE", "BR", "JP", "IN", "NG",
	"FR", "KR" };

/* One range record: the map's value.  `start` is also the key the map
 * holds, in a separate allocation (see table_put). */
struct range {
	uint32_t start, end;
	const char *cc;
	enum action action;
};

/* What a lookup reports, copied out of the read-side. */
struct answer {
	int found;
	uint32_t start, end;
	const char *cc;
	enum action action;
};

/* ---- the feed script and its snapshots ---------------------------- */

enum op_kind { OP_SET, OP_ADD, OP_DEL };

struct op {
	enum op_kind kind;
	struct range r; /* OP_DEL uses r.start only */
};

/* A snapshot: the table's ranges after some version, sorted by start. */
struct snap {
	int n;
	struct range r[MAX_RANGES];
};

static struct range initial[N_BLOCKS];
static int n_initial;
static struct op feed[N_FEED + 1]; /* + the operator's update */
static int n_feed;
static struct snap snaps[N_FEED + 2]; /* snaps[v]: after v updates */

static uint64_t
splitmix64(uint64_t *s)
{
	uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static uint32_t
ip4(unsigned a, unsigned b, unsigned c, unsigned d)
{
	return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | d;
}

static const char *
ipstr(char *buf, size_t len, uint32_t ip)
{
	(void)snprintf(buf, len, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255,
	    (ip >> 8) & 255, ip & 255);
	return buf;
}

/* The index of the range starting at `start` in `s`, or -1. */
static int
snap_find(const struct snap *s, uint32_t start)
{
	int i;

	for (i = 0; i < s->n; i++)
		if (s->r[i].start == start)
			return i;
	return -1;
}

/* Apply one update to a snapshot, keeping it sorted. */
static void
snap_apply(struct snap *s, const struct op *op)
{
	int i = snap_find(s, op->r.start);

	switch (op->kind) {
	case OP_SET:
		s->r[i] = op->r;
		break;
	case OP_DEL:
		memmove(&s->r[i], &s->r[i + 1],
		    (size_t)(s->n - i - 1) * sizeof s->r[0]);
		s->n--;
		break;
	case OP_ADD:
		for (i = s->n; i > 0 && s->r[i - 1].start > op->r.start; i--)
			s->r[i] = s->r[i - 1];
		s->r[i] = op->r;
		s->n++;
		break;
	}
}

/* The answer the table gives for `ip` at snapshot `s`. */
static struct answer
snap_lookup(const struct snap *s, uint32_t ip)
{
	struct answer a = { 0 };
	int i;

	for (i = s->n - 1; i >= 0 && s->r[i].start > ip; i--)
		;
	if (i >= 0 && ip <= s->r[i].end) {
		a.found = 1;
		a.start = s->r[i].start;
		a.end = s->r[i].end;
		a.cc = s->r[i].cc;
		a.action = s->r[i].action;
	}
	return a;
}

static int
answer_eq(const struct answer *x, const struct answer *y)
{
	if (x->found != y->found)
		return 0;
	return !x->found ||
	    (x->start == y->start && x->end == y->end && x->cc == y->cc &&
	        x->action == y->action);
}

/* Push an update onto the feed and record the snapshot after it. */
static void
feed_push(enum op_kind kind, struct range r)
{
	feed[n_feed].kind = kind;
	feed[n_feed].r = r;
	snaps[n_feed + 1] = snaps[n_feed];
	snap_apply(&snaps[n_feed + 1], &feed[n_feed]);
	n_feed++;
}

/*
 * The address plan and the feed.  Each /16 of 10.0.0.0/8 below 10.48 is
 * one allocation, except every seventh, which is unallocated.  The feed
 * reassigns ranges, adds the gaps, withdraws ranges, and splits /16s into
 * /17s, picked by a fixed seed so every run gets the same script.
 */
static void
build_script(void)
{
	struct range r;
	uint64_t rng = 0xed6eac1;
	int b, i;

	for (b = 0; b < N_BLOCKS; b++) {
		if (b % 7 == 3)
			continue;
		r.start = ip4(10, (unsigned)b, 0, 0);
		r.end = ip4(10, (unsigned)b, 255, 255);
		r.cc = countries[b % 8];
		r.action = b % 5 == 0 ? DENY : ALLOW;
		initial[n_initial++] = r;
	}
	snaps[0].n = n_initial;
	memcpy(snaps[0].r, initial, sizeof initial[0] * (size_t)n_initial);

	while (n_feed < N_FEED - 2) {
		const struct snap *cur = &snaps[n_feed];
		uint64_t x = splitmix64(&rng);
		b = (int)(x % N_BLOCKS);
		r.start = ip4(10, (unsigned)b, 0, 0);
		r.end = ip4(10, (unsigned)b, 255, 255);
		r.cc = countries[(x >> 8) % 8];
		r.action = (x >> 16) % 4 == 0 ? DENY : ALLOW;
		i = snap_find(cur, r.start);
		if (i < 0 && snap_find(cur, ip4(10, (unsigned)b, 128, 0)) < 0) {
			feed_push(OP_ADD, r); /* allocate a gap */
		} else if (i < 0) {
			continue; /* a split /16: leave it */
		} else if ((x >> 24) % 4 == 0) {
			feed_push(OP_DEL, r); /* withdraw it */
		} else if ((x >> 24) % 4 == 1 && cur->r[i].end == r.end) {
			struct range lo = r, hi = r; /* split into /17s */
			lo.end = ip4(10, (unsigned)b, 127, 255);
			hi.start = ip4(10, (unsigned)b, 128, 0);
			hi.action = r.action == DENY ? ALLOW : DENY;
			feed_push(OP_DEL, r);
			feed_push(OP_ADD, lo);
			feed_push(OP_ADD, hi);
		} else if (cur->r[i].end == r.end) {
			feed_push(OP_SET, r); /* reassign it */
		}
	}
}

/* ---- the live table ------------------------------------------------ */

static xtc_cskip_t *table;

static int
key_cmp(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return (x > y) - (x < y);
}

/* xtc_rcu_free_fn adapter for xtc_free. */
static void
free_cb(void *p)
{
	xtc_free(p);
}

/*
 * Insert or replace the range starting at r->start.  The map holds its
 * key and value pointers verbatim, so both live in their own
 * allocations.  On a replace the map keeps the key it already has and
 * swaps only the value: the new key was never published and is freed
 * now, while the old value may still be in a reader's hands and is
 * retired.
 */
static int
table_put(const struct range *r)
{
	uint32_t *key = xtc_malloc(sizeof *key);
	struct range *val = xtc_malloc(sizeof *val);
	void *old = NULL;
	int rc;

	if (key == NULL || val == NULL) {
		xtc_free(key);
		xtc_free(val);
		return XTC_E_NOMEM;
	}
	*key = r->start;
	*val = *r;
	if ((rc = xtc_cskip_insert(table, key, val, &old)) != XTC_OK) {
		xtc_free(key);
		xtc_free(val);
		return rc;
	}
	if (old != NULL) {
		xtc_free(key);
		xtc_rcu_retire(old, free_cb);
	}
	return XTC_OK;
}

/* Withdraw the range starting at `start`.  A reader may still be on the
 * node, so its key and value are retired, not freed.  The key is looked
 * up first: the map does not hand it back. */
static int
table_del(uint32_t start)
{
	void *key = NULL, *val = NULL;
	int rc;

	xtc_rcu_read_lock();
	rc = xtc_cskip_floor(table, &start, &key, NULL);
	if (rc == XTC_OK && *(uint32_t *)key != start)
		rc = XTC_E_NOTFOUND;
	xtc_rcu_read_unlock();
	if (rc != XTC_OK)
		return rc;
	if ((rc = xtc_cskip_remove(table, &start, &val)) != XTC_OK)
		return rc;
	xtc_rcu_retire(key, free_cb);
	xtc_rcu_retire(val, free_cb);
	return XTC_OK;
}

static int
table_apply(const struct op *op)
{
	return op->kind == OP_DEL ? table_del(op->r.start) : table_put(&op->r);
}

/* The hot path: which range holds `ip`.  The answer is copied out before
 * the read-side ends; the record pointer is not valid after it. */
static struct answer
table_lookup(uint32_t ip)
{
	struct answer a = { 0 };
	void *val = NULL;

	xtc_rcu_read_lock();
	if (xtc_cskip_floor(table, &ip, NULL, &val) == XTC_OK) {
		const struct range *r = val;
		if (ip <= r->end) {
			a.found = 1;
			a.start = r->start;
			a.end = r->end;
			a.cc = r->cc;
			a.action = r->action;
		}
	}
	xtc_rcu_read_unlock();
	return a;
}

/* Free what the table still holds.  Called after the run, with no
 * readers. */
static void
table_free(void)
{
	void *key = NULL, *val = NULL;
	int rc;

	for (;;) {
		xtc_rcu_read_lock();
		rc = xtc_cskip_min(table, &key, &val);
		xtc_rcu_read_unlock();
		if (rc != XTC_OK)
			break;
		(void)xtc_cskip_remove(table, key, NULL);
		xtc_rcu_retire(key, free_cb);
		xtc_rcu_retire(val, free_cb);
	}
	xtc_cskip_destroy(table);
}

/* ---- the settings -------------------------------------------------- */

static xtc_cfg_ref_t ref_mode, ref_unknown;
static int unknown_changes;

static void
on_unknown_change(const char *name, const void *old_val, const void *new_val,
    void *user)
{
	(void)name;
	(void)old_val;
	(void)new_val;
	(void)user;
	unknown_changes++;
}

static int
settings_init(void)
{
	xtc_cfg_spec_t mode = XTC_CFG_SPEC_INIT, unknown = XTC_CFG_SPEC_INIT;

	mode.name = "edge.mode";
	mode.short_desc = "enforce denies, or audit them (log, let through)";
	mode.kind = XTC_CFG_ENUM;
	mode.dflt.d_enum = MODE_ENFORCE;
	mode.enum_labels = mode_labels;
	mode.n_enum_labels = 2;

	unknown.name = "edge.unknown";
	unknown.short_desc = "verdict for an address no range holds";
	unknown.kind = XTC_CFG_ENUM;
	unknown.dflt.d_enum = ALLOW;
	unknown.enum_labels = unknown_labels;
	unknown.n_enum_labels = 2;
	unknown.on_change = on_unknown_change;

	if (xtc_cfg_register(&mode) != XTC_OK ||
	    xtc_cfg_register(&unknown) != XTC_OK ||
	    xtc_cfg_ref("edge.mode", &ref_mode) != XTC_OK ||
	    xtc_cfg_ref("edge.unknown", &ref_unknown) != XTC_OK)
		return -1;
	return 0;
}

/* The gateway's decision for an answer, under the calling fiber's bound
 * session (or the global settings). */
static enum verdict
decide(const struct answer *a)
{
	int mode = MODE_ENFORCE, unknown = ALLOW;
	enum action act;

	(void)xtc_cfg_ref_get_enum(ref_mode, &mode);
	(void)xtc_cfg_ref_get_enum(ref_unknown, &unknown);
	act = a->found ? a->action : (enum action)unknown;
	if (act == ALLOW)
		return V_ALLOW;
	return mode == MODE_AUDIT ? V_AUDIT : V_DENY;
}

/* ---- the sessions and the feed ------------------------------------ */

/* The scripted requests every session answers, twice. */
#define N_SCRIPT 8
static uint32_t script_ip[N_SCRIPT];

struct session {
	int id;
	int partner;
	xtc_cfg_session_t *cfg;
	uint64_t rng;
	long live, during_feed, bad;
	int64_t progress; /* = live, published (xtc_atomic) */
	enum verdict v[2][N_SCRIPT];
	struct answer a[2][N_SCRIPT];
	enum verdict dry_run, after_dry_run;
	int lower_rc;
	xtc_cfg_source_t mode_src;
	int mode_after_lower;
};

static struct session sessions[N_SESSIONS];
static xtc_barrier_t *barrier;
static uint32_t v_started, v_done; /* feed versions, xtc_atomic */
static int feed_done;              /* xtc_atomic */
static int feed_errors, ops_errors;

/* A random address in 10.0.0.0 .. 10.49.255.255: allocated ranges, the
 * gaps between them, and space past the plan. */
static uint32_t
random_ip(struct session *s)
{
	uint64_t x = splitmix64(&s->rng);

	return ip4(10, (unsigned)(x % 50), (unsigned)(x >> 8) & 255,
	    (unsigned)(x >> 16) & 255);
}

/*
 * One live lookup, checked against the snapshots.  `lo` is the last
 * version the feed finished before the lookup began, `hi` the last it
 * started by the time the lookup ended; the answer must be the table's
 * at one of the versions in between.
 */
static void
live_lookup(struct session *s)
{
	uint32_t ip = random_ip(s), v, lo, hi;
	struct answer got;
	int ok = 0;

	lo = xtc_atomic_load_u32(&v_done, xtc_mo_acquire);
	got = table_lookup(ip);
	hi = xtc_atomic_load_u32(&v_started, xtc_mo_acquire);
	for (v = lo; v <= hi && !ok; v++) {
		struct answer want = snap_lookup(&snaps[v], ip);
		ok = answer_eq(&got, &want);
	}
	s->live++;
	if (lo < (uint32_t)n_feed)
		s->during_feed++;
	if (!ok)
		s->bad++;
}

/* Answer the scripted requests into batch `b`. */
static void
run_script(struct session *s, int b)
{
	int i;

	for (i = 0; i < N_SCRIPT; i++) {
		s->a[b][i] = table_lookup(script_ip[i]);
		s->v[b][i] = decide(&s->a[b][i]);
	}
}

/* A dry-run request: audit for this one request only, then back. */
static void
dry_run(struct session *s, uint32_t ip)
{
	struct answer a = table_lookup(ip);

	(void)xtc_cfg_session_push(s->cfg);
	(void)xtc_cfg_ssn_set_enum(s->cfg, "edge.mode", MODE_AUDIT,
	    XTC_CFG_SRC_SESSION);
	s->dry_run = decide(&a);
	(void)xtc_cfg_session_abort(s->cfg);
	s->after_dry_run = decide(&a);
}

/*
 * One gateway session: bind its settings, serve lookups while the feed
 * runs, then answer the scripted requests before and after the
 * operator's change.  Each barrier wait lines it up with the other
 * sessions and the feed process.
 */
static void
session_proc(void *arg)
{
	struct session *s = arg;
	int i;

	(void)xtc_cfg_session_bind(s->cfg);
	(void)xtc_barrier_wait(barrier); /* 1: feed starts */
	while (!xtc_atomic_load_i32(&feed_done, xtc_mo_acquire)) {
		for (i = 0; i < BATCH; i++)
			live_lookup(s);
		xtc_atomic_store_i64(&s->progress, s->live, xtc_mo_release);
		xtc_yield();
	}
	(void)xtc_barrier_wait(barrier); /* 2: feed is over */
	run_script(s, 0);
	(void)xtc_barrier_wait(barrier); /* 3: batch 1 done */
	(void)xtc_barrier_wait(barrier); /* 4: change made */
	run_script(s, 1);
	if (s->id == 1)
		dry_run(s, script_ip[1]);
	if (s->partner) {
		/* A per-role default (source USER) ranks below the client's
		 * startup option, so it does not displace it. */
		s->lower_rc = xtc_cfg_ssn_set_enum(s->cfg, "edge.mode",
		    MODE_ENFORCE, XTC_CFG_SRC_USER);
		(void)xtc_cfg_session_source(s->cfg, "edge.mode", &s->mode_src);
		(void)xtc_cfg_get_enum("edge.mode", &s->mode_after_lower);
	}
	(void)xtc_cfg_session_bind(NULL);
}

/* Wait until every session has made OVERLAP more lookups, so each
 * table version is read by every loop however the OS schedules the
 * loops' threads. */
static void
feed_wait_for_readers(void)
{
	int64_t mark[N_SESSIONS];
	int i, behind;

	for (i = 0; i < N_SESSIONS; i++)
		mark[i] =
		    xtc_atomic_load_i64(&sessions[i].progress, xtc_mo_acquire);
	do {
		(void)xtc_proc_sleep(FEED_POLL_NS);
		behind = 0;
		for (i = 0; i < N_SESSIONS; i++)
			if (xtc_atomic_load_i64(&sessions[i].progress,
			        xtc_mo_acquire) < mark[i] + OVERLAP)
				behind = 1;
	} while (behind);
}

/*
 * The feed, then the operator.  The feed applies its updates one at a
 * time, publishing each version before and after it touches the table,
 * lets the sessions read each version, and reclaims the retired records
 * as it goes.  Then, between the sessions' two scripted batches, the
 * operator changes the global edge.unknown and pushes one more update:
 * 10.1.0.0/16 is denied.
 */
static void
feed_proc(void *arg)
{
	struct range r = { ip4(10, 1, 0, 0), ip4(10, 1, 255, 255), "DE", DENY };
	int i;

	(void)arg;
	(void)xtc_barrier_wait(barrier); /* 1 */
	for (i = 0; i < n_feed; i++) {
		xtc_atomic_store_u32(&v_started, (uint32_t)i + 1,
		    xtc_mo_release);
		if (table_apply(&feed[i]) != XTC_OK)
			feed_errors++;
		xtc_atomic_store_u32(&v_done, (uint32_t)i + 1, xtc_mo_release);
		if ((i + 1) % RECLAIM_EVERY == 0)
			xtc_rcu_synchronize();
		feed_wait_for_readers();
	}
	xtc_atomic_store_i32(&feed_done, 1, xtc_mo_release);
	(void)xtc_barrier_wait(barrier); /* 2 */
	(void)xtc_barrier_wait(barrier); /* 3 */
	if (xtc_cfg_set_enum("edge.unknown", DENY) != XTC_OK ||
	    table_put(&r) != XTC_OK)
		ops_errors++;
	xtc_rcu_synchronize();
	(void)xtc_barrier_wait(barrier); /* 4 */
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

/* The scripted addresses: an allowed range, a denied range, a range the
 * operator denies, a split range's halves, a gap, and past the plan. */
static void
script_init(void)
{
	script_ip[0] = ip4(10, 2, 17, 9);
	script_ip[1] = ip4(10, 0, 4, 4);
	script_ip[2] = ip4(10, 1, 200, 1);
	script_ip[3] = ip4(10, 3, 0, 1);
	script_ip[4] = ip4(10, 12, 1, 1);
	script_ip[5] = ip4(10, 12, 201, 1);
	script_ip[6] = ip4(10, 33, 99, 99);
	script_ip[7] = ip4(10, 49, 0, 1);
}

/* "10.2.0.0/16 BR deny", or "no range". */
static void
describe(char *buf, size_t len, const struct answer *a)
{
	char ip[16];
	int bits = 32;
	uint32_t size = a->end - a->start;

	while (size != 0) {
		size >>= 1;
		bits--;
	}
	if (!a->found)
		(void)snprintf(buf, len, "no range");
	else
		(void)snprintf(buf, len, "%s/%d %s %s",
		    ipstr(ip, sizeof ip, a->start), bits, a->cc,
		    a->action == DENY ? "deny" : "allow");
}

/* The verdict a session should reach for `a` in batch `b`: partners
 * audit, everyone else enforces, and an unknown address is allowed
 * before the operator's change and denied after it. */
static enum verdict
expected_verdict(const struct answer *a, int partner, int b)
{
	enum action act = a->found ? a->action : (b == 0 ? ALLOW : DENY);

	if (act == ALLOW)
		return V_ALLOW;
	return partner ? V_AUDIT : V_DENY;
}

/* Print batch `b` as one line per scripted address, with what the
 * public and partner sessions decided, and check every session's answer
 * against the frozen table and its verdict against its settings. */
static void
report_batch(int b, const struct snap *frozen)
{
	const struct session *pub = &sessions[0];
	const struct session *par = &sessions[N_SESSIONS - 1];
	char ip[16], what[48];
	int i, j;

	for (i = 0; i < N_SCRIPT; i++) {
		struct answer want = snap_lookup(frozen, script_ip[i]);
		describe(what, sizeof what, &pub->a[b][i]);
		printf("  %-13s in %-22s public: %-5s  partner: %s\n",
		    ipstr(ip, sizeof ip, script_ip[i]), what,
		    verdict_name[pub->v[b][i]], verdict_name[par->v[b][i]]);
		for (j = 0; j < N_SESSIONS; j++) {
			const struct session *s = &sessions[j];
			check(answer_eq(&s->a[b][i], &want),
			    "scripted lookup matches the frozen table");
			check(s->v[b][i] ==
			        expected_verdict(&want, s->partner, b),
			    "verdict follows the session's settings");
		}
	}
}

/* The second batch's table: the feed's last snapshot plus the
 * operator's update. */
static void
report(int verbose)
{
	struct snap after_ops = snaps[n_feed];
	struct op deny = { OP_SET,
		{ ip4(10, 1, 0, 0), ip4(10, 1, 255, 255), "DE", DENY } };
	long bad = 0;
	int i;

	snap_apply(&after_ops, &deny);
	printf("edgeacl: %d loops, %d sessions (%d partner), %d ranges, "
	       "a feed of %d updates\n",
	    N_LOOPS, N_SESSIONS, N_PARTNER, n_initial, n_feed);
	for (i = 0; i < N_SESSIONS; i++) {
		bad += sessions[i].bad;
		check(sessions[i].during_feed >= (long)(n_feed - 1) * OVERLAP,
		    "lookups overlapped every feed update");
		if (verbose)
			printf("  session %d: %ld live lookups, %ld during the "
			       "feed\n",
			    i, sessions[i].live, sessions[i].during_feed);
	}
	printf("live feed: %d updates applied while the sessions looked up "
	       "addresses; %ld inconsistent answers\n",
	    n_feed, bad);
	check(bad == 0, "every live answer matches a table version");
	check(feed_errors == 0 && ops_errors == 0, "feed updates applied");
	check(xtc_cskip_size(table) == (size_t)snaps[n_feed].n,
	    "table size after the feed");

	printf(
	    "batch 1 (edge.unknown=allow; partner sessions edge.mode=audit):\n");
	report_batch(0, &snaps[n_feed]);
	printf("operator: edge.unknown allow -> deny; feed: 10.1.0.0/16 DE "
	       "deny\n");
	printf("batch 2:\n");
	report_batch(1, &after_ops);
	check(unknown_changes == 1, "on_change fired once");

	printf("dry-run request from session 1 for 10.0.4.4: %s, then %s "
	       "once the level is aborted\n",
	    verdict_name[sessions[1].dry_run],
	    verdict_name[sessions[1].after_dry_run]);
	check(sessions[1].dry_run == V_AUDIT &&
	        sessions[1].after_dry_run == V_DENY,
	    "SET LOCAL rolled back");
	printf("partner: edge.mode=enforce from source user: rc %d, mode "
	       "stays %s (source %s)\n",
	    sessions[N_SESSIONS - 1].lower_rc,
	    mode_labels[sessions[N_SESSIONS - 1].mode_after_lower],
	    sessions[N_SESSIONS - 1].mode_src == XTC_CFG_SRC_CLIENT ? "client" :
	                                                              "other");
	for (i = N_SESSIONS - N_PARTNER; i < N_SESSIONS; i++)
		check(sessions[i].mode_after_lower == MODE_AUDIT &&
		        sessions[i].mode_src == XTC_CFG_SRC_CLIENT,
		    "lower-ranked source ignored");
}

/* Create the table and the sessions' settings. */
static int
setup(void)
{
	int i;

	if (settings_init() != 0 || xtc_rcu_init() != XTC_OK ||
	    xtc_cskip_create(key_cmp, &table) != XTC_OK ||
	    xtc_barrier_create(N_SESSIONS + 1, &barrier) != XTC_OK)
		return -1;
	for (i = 0; i < n_initial; i++)
		if (table_put(&initial[i]) != XTC_OK)
			return -1;
	for (i = 0; i < N_SESSIONS; i++) {
		struct session *s = &sessions[i];
		s->id = i;
		s->partner = i >= N_SESSIONS - N_PARTNER;
		s->rng = 0xac10000ULL + (uint64_t)i;
		if (xtc_cfg_session_create(&s->cfg) != XTC_OK)
			return -1;
		if (s->partner &&
		    xtc_cfg_ssn_set_enum(s->cfg, "edge.mode", MODE_AUDIT,
		        XTC_CFG_SRC_CLIENT) != XTC_OK)
			return -1;
	}
	return 0;
}

static void
teardown(void)
{
	int i;

	for (i = 0; i < N_SESSIONS; i++)
		xtc_cfg_session_destroy(sessions[i].cfg);
	table_free();
	xtc_rcu_fini(); /* runs every retired record's free */
	xtc_barrier_destroy(barrier);
	(void)xtc_cfg_unregister("edge.mode");
	(void)xtc_cfg_unregister("edge.unknown");
}

int
main(int argc, char **argv)
{
	xtc_exec_t *exec = NULL;
	int i, verbose = 0;

	if (argc > 1 && strcmp(argv[1], "-v") == 0) {
		verbose = 1;
	} else if (argc > 1) {
		printf(
		    "usage: %s [-v]\n"
		    "Serves IP access lookups from %d sessions on %d loops while "
		    "a feed rewrites the range table (xtc_cskip + xtc_rcu), "
		    "with per-session settings (xtc_cfg); exits 0 iff every "
		    "answer matches the table.  -v prints lookup counts.\n",
		    argv[0], N_SESSIONS, N_LOOPS);
		return strcmp(argv[1], "--help") == 0 ? 0 : 2;
	}

	build_script();
	script_init();
	if (setup() != 0 || xtc_exec_init(&exec, N_LOOPS) != XTC_OK) {
		fprintf(stderr, "edgeacl: setup failed\n");
		return 1;
	}
	for (i = 0; i < N_SESSIONS; i++)
		if (xtc_proc_spawn(xtc_exec_loop(exec, i % N_LOOPS),
		        session_proc, &sessions[i], NULL, NULL) != XTC_OK)
			return 1;
	if (xtc_proc_spawn(xtc_exec_loop(exec, 0), feed_proc, NULL, NULL,
	        NULL) != XTC_OK)
		return 1;
	(void)xtc_exec_run(exec);
	(void)xtc_exec_fini(exec);

	report(verbose);
	teardown();
	if (failures != 0) {
		printf("FAIL: %d check(s) failed\n", failures);
		return 1;
	}
	printf("OK\n");
	return 0;
}
