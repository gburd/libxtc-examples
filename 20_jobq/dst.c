/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/20_jobq/dst.c -- deterministic simulation test of the job
 * queue, using only the public xtc_sim.h.
 *
 *   jobq_dst [-v] [BASE [COUNT]]   run seeds BASE+1 .. BASE+COUNT
 *   jobq_dst [-v] --replay SEED    run one seed, with its trace if -v
 *
 * Each seed is one run of the whole service (jobq.c, unchanged) on the
 * simulator: four loops interleaved on this thread in an order the seed
 * decides, against a virtual clock.  The seed also picks the run's
 * faults, swarm style (each class on or off per seed, so some runs see
 * one fault class alone and others see all of them at once):
 *   - cross-loop message latency, and one-slot message reordering;
 *   - the pessimal scheduler, which starves a loop on purpose;
 *   - clock jitter on every clock read;
 *   - buggify, which turns on some of the service's own pessimal paths
 *     (XTC_SIM_BUGGIFY in jobq.c: a worker pausing past its lease,
 *     short leases, a slow journal, duplicate messages);
 *   - a nemesis proc that kills the queue (a crash and restart from its
 *     journal), kills workers, and cuts a loop off the network for a
 *     while, at times the seed picks.
 *
 * After each run the harness checks the service's invariants
 * (jq_check: no acknowledged job lost, no stale write after a newer one,
 * no two workers under one lease, the stored result is the completed
 * one, every job done) and runs the seed AGAIN: the two runs must agree
 * on the service's trace hash, the simulator's state hash, and the
 * counters.  A seed that fails either check is printed with the command
 * that replays it.
 *
 * Built against a library configured --with-io-backend=sim (libxtc-sim,
 * pkg-config xtc-sim).  Against any other build xtc_sim_exec_run
 * returns XTC_E_NOSYS, and the harness says so and exits 0.
 *
 * Exit status: 0 every seed passed, 1 a seed failed, 2 usage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_inspect.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#include "jobq.h"

#define MS (1000LL * 1000)
#define MAX_STEPS 20000000L /* a run that needs more is not converging */
#define NEMESIS_ACTS 12     /* faults the nemesis injects per run, at most */
#define SITES_MAX 32

/* The faults one seed runs with.  A function of the seed only. */
struct faults {
	int latency;  /* cross-loop message latency, 0.05 .. 2 ms */
	int swizzle;  /* reorder deliveries */
	int pessimal; /* starve loops */
	int jitter;   /* clock jitter, up to 0.5 ms per read */
	int buggify;  /* the service's pessimal paths */
	int kills;    /* the nemesis kills procs */
	int cuts;     /* the nemesis cuts loops off */
};

/* What one run produced, for the replay comparison. */
struct outcome {
	int rc;
	uint64_t trace;
	uint64_t state;
	struct jq_stats stats;
	int acts;
	int tally[3]; /* queue kills, worker kills, cuts */
	char why[256];
};

/* The buggify sites the sweep reached, and how often each was on (over
 * every run, two per seed). */
static struct {
	char name[48];
	int reached, active;
} g_sites[SITES_MAX];
static int g_n_sites;

static struct faults g_f;
static int g_acts;     /* nemesis actions this run */
static int g_tally[3]; /* this run: queue kills, worker kills, cuts */
static struct {
	int tally[3];
	int fenced, regrants, epochs; /* seeds where each happened */
} g_sweep;
static char g_why[256]; /* the consistency check's verdict */
static int g_verbose;

/* splitmix64: the fault mix is derived from the seed before the run,
 * when the simulator's own streams are not active yet. */
static uint64_t
mix(uint64_t x)
{
	x += 0x9E3779B97F4A7C15ULL;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
	return x ^ (x >> 31);
}

static void
pick_faults(uint64_t seed, struct faults *f)
{
	uint64_t r = mix(seed);

	f->latency = (r & 3) != 0; /* 3 seeds in 4 */
	f->swizzle = (r >> 2 & 3) == 0;
	f->pessimal = (r >> 4 & 3) == 0;
	f->jitter = (r >> 6 & 3) == 0;
	f->buggify = (r >> 8 & 3) != 0;
	f->kills = (r >> 10 & 3) != 0;
	f->cuts = (r >> 12 & 1) != 0;
}

static void
set_latency(void)
{
	if (g_f.latency)
		xtc_sim_net_latency(MS / 20, 2 * MS);
}

/* Heal the network.  xtc_sim_partition_clear also clears the latency
 * window, so the run's latency is set again. */
static void
heal(void)
{
	xtc_sim_partition_clear();
	set_latency();
}

/*
 * The nemesis: while the service runs, inject up to NEMESIS_ACTS faults
 * at seeded times, then heal everything and leave, so every seed can
 * reach the end.  Kill: the target unwinds at its next park, and its
 * supervisor restarts it.  Cut: a loop's cross-loop messages are dropped
 * in both directions until the heal.  Loops are named by executor slot
 * + 1 (xtc_sim.h).
 */
static void
nemesis(void *arg)
{
	xtc_pid_t victim;
	int act, loop;

	(void)arg;
	while (g_acts < NEMESIS_ACTS && !jq_finished()) {
		(void)xtc_proc_sleep(2 * MS + (int64_t)xtc_sim_rand(40 * MS));
		act = (int)xtc_sim_rand(3);
		if (act == 0 && g_f.kills) {
			victim = jq_queue_pid();
			if (!xtc_pid_is_none(victim) &&
			    xtc_exit_pid(victim, 9) == XTC_OK) {
				g_tally[0]++;
				jq_trace("nemesis.kill_q", 0, 0);
			}
		} else if (act == 1 && g_f.kills) {
			victim = jq_worker_pid((int)xtc_sim_rand(JQ_WORKERS));
			if (!xtc_pid_is_none(victim) &&
			    xtc_exit_pid(victim, 9) == XTC_OK) {
				g_tally[1]++;
				jq_trace("nemesis.kill_w", 0, 0);
			}
		} else if (act == 2 && g_f.cuts) {
			loop = (int)xtc_sim_rand(JQ_LOOPS);
			xtc_sim_partition_isolate(loop + 1);
			g_tally[2]++;
			jq_trace("nemesis.cut", loop, 0);
			(void)xtc_proc_sleep(
			    5 * MS + (int64_t)xtc_sim_rand(60 * MS));
			heal();
			jq_trace("nemesis.heal", loop, 0);
		}
		g_acts++;
	}
	heal();
}

/* -v: list the procs still alive when a run deadlocked. */
static int
print_proc(const xtc_proc_info_t *pi, void *user)
{
	static const char *const park[] = { "-", "fd", "timer", "mailbox" };

	(void)user;
	printf("  alive: pid %u.%u state %d park %s mailbox %zu kill %d\n",
	    pi->pid.loop_id, pi->pid.local_id, pi->run_state,
	    pi->park_reason >= 0 && pi->park_reason <= 3 ?
	        park[pi->park_reason] :
	        "?",
	    pi->mbox_len, pi->kill_pending);
	return 0;
}

/* Run at quiescence by the simulator (xtc_sim_set_consistency_check):
 * the end-of-test check, with every proc stopped. */
static int
consistency(void *arg)
{
	(void)arg;
	return jq_check(g_why, sizeof g_why);
}

/* Fold this run's buggify sites into the sweep's coverage table. */
static void
note_sites(void)
{
	char name[48];
	int i, k, on, n = xtc_sim_buggify_reached_count();

	for (i = 0; i < n; i++) {
		if (xtc_sim_buggify_site(i, name, sizeof name, &on) != XTC_OK)
			break;
		for (k = 0; k < g_n_sites; k++)
			if (strcmp(g_sites[k].name, name) == 0)
				break;
		if (k == g_n_sites) {
			if (g_n_sites == SITES_MAX)
				continue;
			snprintf(g_sites[k].name, sizeof g_sites[k].name, "%s",
			    name);
			g_n_sites++;
		}
		g_sites[k].reached++;
		g_sites[k].active += on;
	}
}

/*
 * One run of one seed.  Every knob is set before the run and reset
 * after it, so no run inherits another's state: the partition matrix in
 * particular survives xtc_sim_deactivate.
 */
static void
run_seed(uint64_t seed, struct outcome *out)
{
	struct jq_config cfg;
	xtc_exec_t *e = NULL;

	memset(out, 0, sizeof *out);
	pick_faults(seed, &g_f);
	jq_config_default(&cfg);
	g_acts = 0;
	memset(g_tally, 0, sizeof g_tally);
	g_why[0] = '\0';
	if ((out->rc = xtc_exec_init(&e, JQ_LOOPS)) != XTC_OK)
		return;
	if ((out->rc = jq_start(e, &cfg)) != XTC_OK ||
	    (out->rc = xtc_proc_spawn(xtc_exec_loop(e, 1), nemesis, NULL, NULL,
	         NULL)) != XTC_OK) {
		(void)xtc_exec_fini(e);
		return;
	}
	xtc_sim_partition_clear();
	set_latency();
	if (g_f.swizzle)
		xtc_sim_swizzle_enable(100);
	if (g_f.pessimal)
		xtc_sim_sched_pessimal(200);
	if (g_f.jitter)
		xtc_sim_clock_skew(0, (int)(MS / 2));
	if (g_f.buggify)
		xtc_sim_buggify_enable(400);
	xtc_sim_set_consistency_check(consistency, NULL);

	out->rc = xtc_sim_exec_run(e, seed, MAX_STEPS);
	if (out->rc == XTC_E_DEADLK && g_verbose)
		(void)xtc_inspect_procs(print_proc, NULL);

	note_sites();
	xtc_sim_buggify_disable();
	xtc_sim_swizzle_disable();
	xtc_sim_partition_clear();
	out->trace = jq_trace_hash();
	out->state = xtc_sim_state_hash(e);
	out->stats = *jq_stats();
	out->acts = g_acts;
	memcpy(out->tally, g_tally, sizeof out->tally);
	jq_finish();
	(void)xtc_exec_fini(e);
	if (out->rc != XTC_OK && g_why[0] == '\0' &&
	    jq_check(g_why, sizeof g_why) == XTC_OK)
		snprintf(g_why, sizeof g_why, "the run ended with %s",
		    xtc_strerror(out->rc));
	snprintf(out->why, sizeof out->why, "%s", g_why);
}

static int
same(const struct outcome *a, const struct outcome *b)
{
	return a->rc == b->rc && a->trace == b->trace && a->state == b->state &&
	    a->acts == b->acts &&
	    memcmp(a->tally, b->tally, sizeof a->tally) == 0 &&
	    memcmp(&a->stats, &b->stats, sizeof a->stats) == 0;
}

static void
print_faults(void)
{
	printf("  faults: latency=%d swizzle=%d pessimal=%d jitter=%d "
	       "buggify=%d kills=%d cuts=%d\n",
	    g_f.latency, g_f.swizzle, g_f.pessimal, g_f.jitter, g_f.buggify,
	    g_f.kills, g_f.cuts);
}

/* Add one passing seed to the sweep's coverage totals. */
static void
tally(const struct outcome *o)
{
	int k;

	for (k = 0; k < 3; k++)
		g_sweep.tally[k] += o->tally[k];
	g_sweep.fenced += o->stats.fenced > 0;
	g_sweep.regrants += o->stats.regrants > 0;
	g_sweep.epochs += o->stats.epochs > 1;
}

/* Run seed twice: the invariants, then the replay.  0 or 1.  With -v
 * only the first run prints its trace. */
static int
check_seed(const char *prog, uint64_t seed)
{
	struct outcome a, b;
	unsigned long long s = (unsigned long long)seed;

	run_seed(seed, &a);
	if (a.rc != XTC_OK) {
		printf("FAIL seed %llu: %s\n", s, a.why);
		print_faults();
		printf("  replay: %s -v --replay %llu\n", prog, s);
		return 1;
	}
	jq_set_verbose(0);
	run_seed(seed, &b);
	jq_set_verbose(g_verbose);
	if (!same(&a, &b)) {
		printf("FAIL seed %llu: NONDETERMINISM: the replay differs "
		       "(trace %016llx vs %016llx, state %016llx vs %016llx)\n",
		    s, (unsigned long long)a.trace, (unsigned long long)b.trace,
		    (unsigned long long)a.state, (unsigned long long)b.state);
		print_faults();
		printf("  replay: %s -v --replay %llu\n", prog, s);
		return 1;
	}
	tally(&a);
	return 0;
}

static void
report(uint64_t n)
{
	int k;

	printf("OK: %llu seeds, each run twice and replayed identically\n",
	    (unsigned long long)n);
	printf("  nemesis: %d queue kills, %d worker kills, %d network cuts\n",
	    g_sweep.tally[0], g_sweep.tally[1], g_sweep.tally[2]);
	printf("  seeds where the queue restarted %d, a lease was granted "
	       "again %d, the store fenced a stale write %d\n",
	    g_sweep.epochs, g_sweep.regrants, g_sweep.fenced);
	for (k = 0; k < g_n_sites; k++)
		printf("  buggify %-28s reached in %5d runs, on in %5d\n",
		    g_sites[k].name, g_sites[k].reached, g_sites[k].active);
}

static int
usage(const char *prog, int rc)
{
	printf("usage: %s [-v] [BASE [COUNT]]\n"
	       "       %s [-v] --replay SEED\n"
	       "  Runs seeds BASE+1 .. BASE+COUNT (default 0 300) of the job "
	       "queue under\n"
	       "  the simulator, each twice, and checks its invariants and "
	       "the replay.\n"
	       "  -v prints the trace of every run.\n",
	    prog, prog);
	return rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0, count = 300, i;
	struct outcome probe;
	int a = 1, replay = 0;

	if (a < argc && strcmp(argv[a], "--help") == 0)
		return usage(argv[0], 0);
	if (a < argc && strcmp(argv[a], "-v") == 0) {
		jq_set_verbose(1);
		g_verbose = 1;
		a++;
	}
	if (a < argc && strcmp(argv[a], "--replay") == 0) {
		if (a + 2 != argc)
			return usage(argv[0], 2);
		base = strtoull(argv[a + 1], NULL, 0) - 1;
		count = 1;
		replay = 1;
	} else {
		if (a < argc)
			base = strtoull(argv[a++], NULL, 0);
		if (a < argc)
			count = strtoull(argv[a++], NULL, 0);
		if (a != argc)
			return usage(argv[0], 2);
	}

	run_seed(base + 1, &probe);
	if (probe.rc == XTC_E_NOSYS) {
		printf("not a sim build (xtc_sim_exec_run: %s): link "
		       "libxtc-sim to simulate\n",
		    xtc_strerror(probe.rc));
		return 0;
	}
	if (replay) {
		print_faults();
		if (check_seed(argv[0], base + 1) != 0)
			return 1;
		printf("OK: seed %llu passed and replayed identically "
		       "(trace %016llx)\n",
		    (unsigned long long)(base + 1),
		    (unsigned long long)probe.trace);
		return 0;
	}
	for (i = 1; i <= count; i++)
		if (check_seed(argv[0], base + i) != 0)
			return 1;
	report(count);
	return 0;
}
