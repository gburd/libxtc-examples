/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/18_clickstream/clickstream.c -- web analytics over the access
 * logs of several front-end servers: a lazy stream pipeline per log, a
 * Bloom filter that drops redelivered lines, and HyperLogLog sketches
 * that count distinct users across all the servers.
 *
 * Each front end's log is read by its own proc, one per loop of an
 * xtc_exec, through an xtc_stream pipeline:
 *
 *   log lines -> parse -> drop malformed -> drop bots and health checks
 *             -> drop lines seen before -> aggregate
 *
 * The stages are xtc_stream_map / _filter over a source made with
 * xtc_stream_create, drained by xtc_stream_for_each.  The pipeline is
 * pull-based and lazy: one line is in flight at a time, so a shard's
 * memory does not grow with the log.
 *
 * The log shipper delivers at least once: after a timeout it resends the
 * line it already sent, so about 3% of lines arrive twice.  Counting them
 * twice inflates every number.  An exact "seen before" set grows with
 * the log; the Bloom filter (xtc_bloom) is fixed-size.  It never misses
 * a repeat, but now and then it flags a new line as seen and drops it,
 * at the rate it was sized for (1% here).  That trade is fine for
 * analytics and wrong for billing.
 *
 * "How many distinct users today" is the classic HyperLogLog question
 * (xtc_hll).  Users hit several front ends, so per-server distinct counts
 * do not add up.  Each shard keeps its own 4 KiB sketch, and main merges
 * them (register-wise max) into the sketch of the union.  The merge is
 * exact, the shards share nothing while they run, and the prob
 * structures need no locks: xtc_prob(3) recommends exactly this pattern.
 *
 * The generator also keeps an exact oracle of what the pipeline should
 * find: bitmaps of the users and the checkout users, and counts of lines
 * by kind.  main compares the pipeline against it: every redelivery
 * dropped, the lines lost to Bloom false positives under the configured
 * rate, and the sketch estimates within four standard errors.  A real
 * service has no oracle; that is why it uses the sketches.
 *
 * The output is deterministic: the generator is seeded per shard, the
 * hashes are fixed, and an HLL merge does not depend on order.
 *
 * Uses only the public xtc_* API (this is a consumer program).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_prob.h"
#include "xtc_proc.h"
#include "xtc_stream.h"

#define N_SHARDS 4      /* front-end servers, one loop each */
#define N_LINES 50000   /* log lines per front end */
#define N_USERS 20000   /* user ids u00000 .. u19999 */
#define HLL_P 12        /* 4096 registers: about 1.6% error */
#define BLOOM_FP 0.01   /* target false-positive rate */
#define REDELIVER_PCT 3 /* lines the shipper sends twice */
#define LINE_MAX 96

/* What the pipeline extracts from one log line.  The string fields point
 * into the shard's line buffer, valid until the next pull. */
struct hit {
	int valid;
	uint64_t req_id;
	const char *user;
	size_t user_len;
	int checkout; /* path is /checkout */
	int health;   /* path is /healthz */
	int status;
	int human; /* agent "web" (not "bot" or "lb") */
};

/* One front end: its log generator and oracle, its pipeline state, and
 * its results. */
struct shard {
	int id;
	uint64_t rng;
	int line_no;
	char line[LINE_MAX];
	int line_is_repeat; /* oracle: this line is a redelivery */
	int line_malformed; /* oracle: truncated in transit */
	int line_countable; /* oracle: a person's page view */
	struct hit hit;

	xtc_bloom_t *seen;
	xtc_hll_t *users;
	xtc_hll_t *buyers;

	/* Pipeline counters. */
	int malformed, filtered, repeats_dropped, unique_lost, repeats_missed;
	int counted;

	/* The oracle. */
	int o_malformed, o_countable, o_countable_repeats;
	uint8_t o_users[N_USERS / 8];
	uint8_t o_buyers[N_USERS / 8];
};

static struct shard shards[N_SHARDS];

/* ---- the log generator (the stream's source) ---------------------- */

static uint64_t
splitmix64(uint64_t *s)
{
	uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static void
bit_set(uint8_t *bits, int i)
{
	bits[i / 8] |= (uint8_t)(1u << (i % 8));
}

static int
bit_count(const uint8_t *bits)
{
	int i, n = 0;

	for (i = 0; i < N_USERS; i++)
		n += (bits[i / 8] >> (i % 8)) & 1;
	return n;
}

/*
 * Write one new log line for shard `s` into s->line and record in the
 * oracle what it is.  The traffic: mostly people browsing, 2% of their
 * page views a checkout; a crawler; the load balancer's health checks;
 * and now and then a line truncated in transit.  The request id is the
 * front end's number and the line number, so ids are unique across the
 * fleet.
 */
static void
gen_line(struct shard *s)
{
	static const char *const pages[] = { "/", "/search", "/item/17",
		"/item/42", "/cart" };
	uint64_t who = splitmix64(&s->rng), what = splitmix64(&s->rng);
	unsigned long long req_id;
	int user, status, kind = (int)(who % 100), len;
	const char *path, *agent;

	/* A fifth of the traffic comes from 500 heavy users. */
	user = (who >> 8) % 5 == 0 ? (int)((who >> 16) % 500) :
	                             (int)((who >> 16) % N_USERS);
	path = what % 100 < 2 ? "/checkout" : pages[(what >> 8) % 5];
	status =
	    (what >> 16) % 50 == 0 ? 500 : ((what >> 16) % 10 == 0 ? 404 : 200);
	agent = "web";
	if (kind < 6) {
		path = "/healthz";
		agent = "lb";
		user = -1;
	} else if (kind < 12) {
		agent = "bot";
	}
	req_id = ((unsigned long long)s->id << 32) | (unsigned)s->line_no;
	if (user < 0)
		len = snprintf(s->line, sizeof s->line,
		    "%016llx - GET %s %d %s", req_id, path, status, agent);
	else
		len = snprintf(s->line, sizeof s->line,
		    "%016llx u%05d GET %s %d %s", req_id, user, path, status,
		    agent);

	s->line_countable = 0;
	s->line_malformed = (what >> 32) % 200 == 0;
	if (s->line_malformed) {
		/* Cut it at least two bytes short: it loses its agent. */
		s->line[8 + (what >> 40) % (unsigned)(len - 9)] = '\0';
		return;
	}
	if (strcmp(agent, "web") != 0)
		return;
	s->line_countable = 1;
	s->o_countable++;
	bit_set(s->o_users, user);
	if (strcmp(path, "/checkout") == 0 && status == 200)
		bit_set(s->o_buyers, user);
}

/* The stream source: the next line of this front end's log, or
 * XTC_E_NOTFOUND at the end.  A redelivery repeats the line before it,
 * which is still in the buffer. */
static int
log_next(void *ctx, void **out)
{
	struct shard *s = ctx;
	int repeat;

	if (s->line_no == N_LINES)
		return XTC_E_NOTFOUND;
	repeat = s->line_no > 0 && splitmix64(&s->rng) % 100 < REDELIVER_PCT;
	s->line_is_repeat = repeat;
	if (repeat) {
		if (s->line_countable)
			s->o_countable_repeats++;
	} else {
		gen_line(s);
	}
	if (s->line_malformed)
		s->o_malformed++;
	s->line_no++;
	*out = s->line;
	return XTC_OK;
}

/* ---- the pipeline stages ------------------------------------------ */

/* Split off the next space-separated field of *p; NULL if there is none. */
static const char *
field(const char **p, size_t *len)
{
	const char *start = *p, *end;

	if (*start == '\0')
		return NULL;
	end = strchr(start, ' ');
	if (end == NULL)
		end = start + strlen(start);
	*len = (size_t)(end - start);
	*p = *end == ' ' ? end + 1 : end;
	return *len > 0 ? start : NULL;
}

/* map: parse a log line into the shard's hit.  The line must have all six
 * fields, a 16-digit hex request id, a three-digit status and a known
 * agent; anything else is malformed. */
static void *
parse(void *v, void *user)
{
	struct shard *s = user;
	struct hit *h = &s->hit;
	const char *p = v, *f[6];
	unsigned long long req_id;
	size_t n[6];
	int i;

	memset(h, 0, sizeof *h);
	for (i = 0; i < 6; i++)
		if ((f[i] = field(&p, &n[i])) == NULL)
			return h;
	if (*p != '\0' || n[0] != 16 || n[4] != 3 ||
	    sscanf(f[0], "%16llx", &req_id) != 1 ||
	    sscanf(f[4], "%3d", &h->status) != 1)
		return h;
	h->req_id = req_id;
	h->human = n[5] == 3 && memcmp(f[5], "web", 3) == 0;
	if (!h->human && !(n[5] == 3 && memcmp(f[5], "bot", 3) == 0) &&
	    !(n[5] == 2 && memcmp(f[5], "lb", 2) == 0))
		return h;
	h->user = f[1];
	h->user_len = n[1];
	h->checkout = n[3] == 9 && memcmp(f[3], "/checkout", 9) == 0;
	h->health = n[3] == 8 && memcmp(f[3], "/healthz", 8) == 0;
	h->valid = 1;
	return h;
}

/* filter: keep well-formed lines. */
static int
well_formed(void *v, void *user)
{
	struct hit *h = v;
	struct shard *s = user;

	if (!h->valid)
		s->malformed++;
	return h->valid;
}

/* filter: keep people; drop crawlers and the load balancer's checks. */
static int
human(void *v, void *user)
{
	struct hit *h = v;
	struct shard *s = user;

	if (!h->human || h->health) {
		s->filtered++;
		return 0;
	}
	return 1;
}

/*
 * filter: drop a line whose request id the Bloom filter has seen.  The
 * oracle's flag for the line in flight says whether it really was a
 * redelivery, so the shard can count the filter's two kinds of error:
 * a repeat it missed (never, for a Bloom filter) and a new line it
 * dropped (a false positive).
 */
static int
first_delivery(void *v, void *user)
{
	struct hit *h = v;
	struct shard *s = user;

	if (xtc_bloom_maybe_contains(s->seen, &h->req_id, sizeof h->req_id)) {
		if (s->line_is_repeat)
			s->repeats_dropped++;
		else
			s->unique_lost++;
		return 0;
	}
	if (s->line_is_repeat)
		s->repeats_missed++;
	xtc_bloom_add(s->seen, &h->req_id, sizeof h->req_id);
	return 1;
}

/* for_each: count the hit, its user, and its user again if they checked
 * out. */
static int
aggregate(void *v, void *user)
{
	struct hit *h = v;
	struct shard *s = user;

	s->counted++;
	xtc_hll_add(s->users, h->user, h->user_len);
	if (h->checkout && h->status == 200)
		xtc_hll_add(s->buyers, h->user, h->user_len);
	return 0;
}

/* ---- the shard proc ----------------------------------------------- */

/* Build the pipeline for one front end and drain it. */
static void
shard_proc(void *arg)
{
	struct shard *s = arg;
	xtc_stream_t *st = NULL, *next = NULL;

	if (xtc_stream_create(log_next, s, &st) != XTC_OK)
		return;
	if (xtc_stream_map(st, parse, s, &next) != XTC_OK)
		goto out;
	st = next;
	if (xtc_stream_filter(st, well_formed, s, &next) != XTC_OK)
		goto out;
	st = next;
	if (xtc_stream_filter(st, human, s, &next) != XTC_OK)
		goto out;
	st = next;
	if (xtc_stream_filter(st, first_delivery, s, &next) != XTC_OK)
		goto out;
	st = next;
	(void)xtc_stream_for_each(st, aggregate, s);
out:
	xtc_stream_destroy(st); /* the outermost stage frees them all */
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

/* Print an estimate beside the exact value, and check that it is within
 * four standard errors (1.04 / sqrt(2^p) each). */
static void
report_estimate(const char *what, uint64_t est, int exact)
{
	double err = exact > 0 ? ((double)est - exact) * 100.0 / exact : 0;
	double bound = 4 * 104.0 / 64; /* 4 x 1.04/sqrt(4096), percent */

	printf("%s: estimate %llu, exact %d (%+.2f%%)\n", what,
	    (unsigned long long)est, exact, err);
	check(err <= bound && err >= -bound, what);
}

/* One line per front end, with its counters checked against its oracle. */
static void
report_shard(const struct shard *s)
{
	printf("front end %d: %d lines: %d malformed, %d bot/health, "
	       "%d redelivered dropped, %d new lost, %d counted\n",
	    s->id, N_LINES, s->malformed, s->filtered, s->repeats_dropped,
	    s->unique_lost, s->counted);
	check(s->malformed == s->o_malformed, "malformed lines");
	check(s->repeats_missed == 0, "no redelivery counted twice");
	check(s->repeats_dropped == s->o_countable_repeats,
	    "every redelivery dropped");
	check(s->counted + s->unique_lost == s->o_countable,
	    "every new line counted or lost");
	check(s->malformed + s->filtered + s->repeats_dropped + s->unique_lost +
	            s->counted ==
	        N_LINES,
	    "lines add up");
}

/* Merge the shards' sketches and the oracles, print the totals, and check
 * them. */
static void
report_totals(xtc_hll_t *users, xtc_hll_t *buyers)
{
	uint8_t all_users[N_USERS / 8] = { 0 }, all_buyers[N_USERS / 8] = { 0 };
	int i, j, lost = 0, countable = 0, per_shard_sum = 0;
	int exact_users, exact_buyers;

	for (i = 0; i < N_SHARDS; i++) {
		const struct shard *s = &shards[i];
		check(xtc_hll_merge(users, s->users) == XTC_OK, "merge users");
		check(xtc_hll_merge(buyers, s->buyers) == XTC_OK,
		    "merge buyers");
		for (j = 0; j < N_USERS / 8; j++) {
			all_users[j] |= s->o_users[j];
			all_buyers[j] |= s->o_buyers[j];
		}
		per_shard_sum += (int)xtc_hll_count(s->users);
		lost += s->unique_lost;
		countable += s->o_countable;
	}
	exact_users = bit_count(all_users);
	exact_buyers = bit_count(all_buyers);

	report_estimate("distinct users", xtc_hll_count(users), exact_users);
	printf("  (adding the %d per-front-end estimates gives %d: a user "
	       "seen by several front ends counts several times)\n",
	    N_SHARDS, per_shard_sum);
	report_estimate("distinct users who checked out", xtc_hll_count(buyers),
	    exact_buyers);
	printf("conversion: %.1f%% estimated, %.1f%% exact\n",
	    100.0 * (double)xtc_hll_count(buyers) /
	        (double)xtc_hll_count(users),
	    100.0 * exact_buyers / exact_users);
	printf("Bloom filter: %d of %d new lines lost to false positives "
	       "(%.3f%%, sized for %.0f%%)\n",
	    lost, countable, 100.0 * lost / countable, 100 * BLOOM_FP);
	check(per_shard_sum > exact_users, "per-front-end sum overcounts");
	check((double)lost / countable <= BLOOM_FP, "false positive rate");
}

/* Create the shards' filters and sketches.  The Bloom filter is sized
 * for the lines one front end logs. */
static int
shards_init(void)
{
	int i;

	for (i = 0; i < N_SHARDS; i++) {
		struct shard *s = &shards[i];
		s->id = i;
		s->rng = 0x5eed0000u + (uint64_t)i;
		if (xtc_bloom_create(N_LINES, BLOOM_FP, &s->seen) != XTC_OK ||
		    xtc_hll_create(HLL_P, &s->users) != XTC_OK ||
		    xtc_hll_create(HLL_P, &s->buyers) != XTC_OK)
			return -1;
	}
	return 0;
}

static void
shards_fini(void)
{
	int i;

	for (i = 0; i < N_SHARDS; i++) {
		xtc_bloom_destroy(shards[i].seen);
		xtc_hll_destroy(shards[i].users);
		xtc_hll_destroy(shards[i].buyers);
	}
}

int
main(int argc, char **argv)
{
	xtc_exec_t *exec = NULL;
	xtc_hll_t *users = NULL, *buyers = NULL;
	int i;

	if (argc > 1) {
		printf(
		    "usage: %s\n"
		    "Runs a stream pipeline over %d synthetic front-end access "
		    "logs (%d lines each), drops redelivered lines with a Bloom "
		    "filter, counts distinct users with merged HyperLogLog "
		    "sketches; exits 0 iff the results match an exact "
		    "oracle within the structures' error bounds.\n",
		    argv[0], N_SHARDS, N_LINES);
		return strcmp(argv[1], "--help") == 0 ? 0 : 2;
	}

	if (shards_init() != 0 || xtc_hll_create(HLL_P, &users) != XTC_OK ||
	    xtc_hll_create(HLL_P, &buyers) != XTC_OK ||
	    xtc_exec_create(&(xtc_exec_opts_t){
	        .size = sizeof(xtc_exec_opts_t), .n_loops = N_SHARDS }, &exec) != XTC_OK) {
		fprintf(stderr, "clickstream: init failed\n");
		return 1;
	}
	for (i = 0; i < N_SHARDS; i++) {
		if (xtc_proc_spawn(xtc_exec_loop(exec, i), shard_proc,
		        &shards[i], NULL, NULL) != XTC_OK) {
			fprintf(stderr, "clickstream: spawn failed\n");
			return 1;
		}
	}
	(void)xtc_exec_run(exec);
	xtc_exec_destroy(exec);

	printf("clickstream: %d front ends x %d log lines, %d user ids\n",
	    N_SHARDS, N_LINES, N_USERS);
	for (i = 0; i < N_SHARDS; i++)
		report_shard(&shards[i]);
	report_totals(users, buyers);

	xtc_hll_destroy(users);
	xtc_hll_destroy(buyers);
	shards_fini();
	if (failures != 0) {
		printf("FAIL: %d check(s) failed\n", failures);
		return 1;
	}
	printf("OK\n");
	return 0;
}
