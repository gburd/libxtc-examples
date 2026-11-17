/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/20_jobq/jobq.c -- a job queue with leases, retries and fencing
 * tokens.
 *
 * A producer submits jobs to a queue.  Workers lease a job, do the work,
 * write the job's result to an object store, and tell the queue the job
 * is complete.  A lease expires: a worker that dies or stalls loses its
 * job, and the queue grants it to another worker.  So a job may run more
 * than once, and a worker that stalled past its lease may wake up and
 * write a result after the job's new owner did.  Every lease therefore
 * carries a fencing token that grows with each grant, and the store
 * refuses a write whose token is lower than one it has already accepted
 * for that job.  Execution is at least once; the store's effects are
 * fenced, so a stale holder never overwrites a newer one.
 *
 * Placement (four loops):
 *   loop 0   the queue, under a supervisor on the same loop
 *   loop 1   the object store and the producer
 *   loop 2   workers 0 and 1, under a supervisor
 *   loop 3   workers 2 and 3, under a supervisor
 * Every supervisor runs on its children's loop.  A child's DOWN then
 * never crosses loops, which matters under a simulated partition: the
 * simulator drops a DOWN on a cut edge for good (see the README).
 *
 * The queue keeps a journal: an in-memory record standing for a file the
 * queue fsyncs.  A journal write is a sleep of journal_ns (the fsync)
 * followed by the update; a queue killed during the sleep never made the
 * update.  The journal holds the queue's epoch, each job's state and the
 * token that completed it.  Leases are not journaled: a restarted queue
 * bumps its epoch first (durably), forgets every lease, and grants new
 * ones with tokens (epoch << 32 | n) above any the old incarnation
 * granted.
 *
 * Clients find the queue and the store by name in an xtc_reg registry,
 * because the queue's pid changes when it restarts.  Every request
 * carries the client's pid and a request id; a client waits RPC_NS for
 * the matching reply and retries on silence.  Every request is
 * idempotent, so a retry after a lost reply is safe.
 *
 * Decision points are marked with XTC_SIM_BUGGIFY, so under the
 * simulator a seeded run takes the pessimal path there: a worker that
 * stalls between checking its lease and writing, leases far shorter than
 * the work, a slow journal, a duplicated submit or completion.  In a
 * normal build each mark is a load of a flag that is never set.
 *
 * Uses only the public xtc_* API.
 */

#include <stdio.h>
#include <string.h>

/* -DPLANT=n builds in planted bug n (-DPLANT alone: bug 1). */
#if defined(PLANT) && !defined(XTC_DST_INJECT_BUG)
#define XTC_DST_INJECT_BUG PLANT
#endif

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_orc.h"
#include "xtc_proc.h"
#include "xtc_reg.h"
#include "xtc_sim.h"

#include "jobq.h"

#define MS (1000LL * 1000)
#define RPC_NS (10 * MS) /* a client waits this long for a reply */
#define POLL_NS (2 * MS) /* an idle worker asks again after this */
#define WORK_NS (3 * MS) /* a job takes 1..3 ms */
#define ATTEMPTS 4       /* tries of one store write or completion */
#define LOG_MAX 4096     /* the store's write log, for the checks */

/*
 * Planted bugs, for the harness's bug-detection check.  Each is compiled
 * in only with -DXTC_DST_INJECT_BUG=n (make PLANT=n); see xtc_sim(3).
 */
#define JQ_BUG_NOFENCE 1  /* the store accepts a write without the fence */
#define JQ_BUG_EARLYACK 2 /* the queue acks a submit before it is durable */
#define JQ_BUG_EPOCH 3    /* a restarted queue reuses its epoch */

enum jq_msg_type {
	M_SUBMIT = 1, /* producer -> queue: job */
	M_LEASE,      /* worker -> queue */
	M_COMPLETE,   /* worker -> queue: job, token */
	M_STATUS,     /* producer -> queue: job */
	M_WRITE,      /* worker -> store: job, token, worker */
	M_ACK,        /* replies */
	M_NONE,       /* no job to lease */
	M_GRANT,      /* job, token, deadline */
	M_STALE,      /* the completion's lease is not current */
	M_FENCED,     /* the store refused a stale token */
	M_STATE       /* status: the job's JS_* state */
};

enum { JS_NONE, JS_READY, JS_DONE };

struct jq_msg {
	int type;
	int job;
	int status;       /* M_STATE: a JS_* value */
	int worker;       /* M_WRITE: who writes */
	uint64_t token;   /* the lease's fencing token */
	uint64_t rid;     /* request id, copied into the reply */
	int64_t deadline; /* M_GRANT: when the lease ends */
	xtc_pid_t from;   /* where the reply goes */
};

/* The queue's journal: what survives a queue crash. */
static struct {
	uint32_t epoch;
	int state[JQ_JOBS_MAX];
	uint64_t done_token[JQ_JOBS_MAX];
} g_journal;

/* The queue's volatile table, rebuilt from the journal at each start. */
static struct qjob {
	int state;
	uint64_t token;   /* the current lease, 0 if none was granted */
	int64_t deadline; /* the current lease's end */
} g_q[JQ_JOBS_MAX];
static uint32_t g_epoch;   /* this incarnation's epoch */
static uint32_t g_granted; /* leases granted in this epoch */

/* The object store: the job results' system of record. */
static struct {
	uint64_t fence[JQ_JOBS_MAX]; /* the highest token accepted per job */
	int n_log;
	int overflow;
	struct {
		int job, worker;
		uint64_t token;
	} log[LOG_MAX];
} g_store;

/* The producer's record of what it was told. */
static int g_acked[JQ_JOBS_MAX]; /* the queue acknowledged the submit */
static int g_lost[JQ_JOBS_MAX];  /* the queue later did not know the job */
static int g_seen[JQ_JOBS_MAX];  /* the producer saw the job done */
static int g_gave_up;
static int g_finished; /* xtc_atomic: the producer is done */

static struct jq_config g_cfg;
static struct jq_stats g_stats;
static xtc_reg_t *g_reg;
static xtc_supervisor_t *g_sup[3]; /* the queue's, then the workers' */
static xtc_atomic_pid_t g_worker[JQ_WORKERS];
static xtc_atomic_pid_t g_store_pid;
static uint64_t g_next_rid; /* xtc_atomic */
static uint64_t g_trace;
static int g_verbose;

void
jq_config_default(struct jq_config *cfg)
{
	cfg->n_jobs = 24;
	cfg->lease_ns = 20 * MS;
	cfg->journal_ns = 1 * MS;
	cfg->give_up_ns = 10000 * MS;
}

/*
 * Fold one state change into the trace hash (FNV-1a over the event name,
 * its two values and the virtual time), and print it in verbose mode.
 * Only under the simulator: there the clock and the order of events are
 * functions of the seed, so two runs of one seed must produce the same
 * hash.  On real threads neither holds, and the hash is not kept.
 */
void
jq_trace(const char *ev, int a, uint64_t b)
{
	uint64_t h = g_trace ? g_trace : 1469598103934665603ULL;
	uint64_t v[3];
	int64_t now;
	int i;

	if (!xtc_sim_active())
		return;
	now = xtc_clock_mono();
	for (i = 0; ev[i] != '\0'; i++)
		h = (h ^ (unsigned char)ev[i]) * 1099511628211ULL;
	v[0] = (uint64_t)(int64_t)a;
	v[1] = b;
	v[2] = (uint64_t)now;
	for (i = 0; i < 3; i++)
		h = (h ^ v[i]) * 1099511628211ULL;
	g_trace = h;
	if (g_verbose)
		printf("%10.3f ms  %-16s %3d  %llx\n", (double)now / MS, ev, a,
		    (unsigned long long)b);
}

void
jq_set_verbose(int on)
{
	g_verbose = on;
}

uint64_t
jq_trace_hash(void)
{
	return g_trace;
}

const struct jq_stats *
jq_stats(void)
{
	return &g_stats;
}

int
jq_finished(void)
{
	return xtc_atomic_load_i32(&g_finished, xtc_mo_acquire);
}

static xtc_pid_t
lookup(const char *name)
{
	xtc_pid_t pid = XTC_PID_NONE;

	(void)xtc_reg_get(g_reg, name, &pid);
	return pid;
}

xtc_pid_t
jq_queue_pid(void)
{
	return lookup("jobq");
}

xtc_pid_t
jq_worker_pid(int i)
{
	return xtc_atomic_pid_load(&g_worker[i]);
}

/* ---- messages ----------------------------------------------------- */

/* Receive one jq_msg, dropping anything else.  XTC_E_AGAIN on timeout. */
static int
recv_msg(struct jq_msg *m, int64_t timeout_ns)
{
	void *buf = NULL;
	size_t n = 0;
	int rc;

	rc = xtc_recv(&buf, &n, timeout_ns);
	if (rc != XTC_OK)
		return rc;
	if (n == sizeof *m)
		memcpy(m, buf, sizeof *m);
	xtc_free(buf);
	return n == sizeof *m ? XTC_OK : XTC_E_AGAIN;
}

/* Answer req.  A lost reply is the client's to retry, so a failed send
 * is not an error here. */
static void
reply(const struct jq_msg *req, int type)
{
	struct jq_msg r = *req;

	r.type = type;
	(void)xtc_send(req->from, &r, sizeof r);
}

/*
 * Send req to the proc registered as name and wait for its reply.
 * XTC_OK with *rep filled, or XTC_E_AGAIN when no reply came within
 * RPC_NS: the request or the reply was lost, or the server is
 * restarting.  A late reply to an earlier attempt is dropped by its
 * request id.
 */
static int
rpc(const char *name, struct jq_msg *req, struct jq_msg *rep)
{
	xtc_pid_t to = lookup(name);
	int64_t deadline, left;

	req->rid = xtc_atomic_fetch_add_u64(&g_next_rid, 1, xtc_mo_relaxed);
	req->from = xtc_self();
	if (xtc_pid_is_none(to) || xtc_send(to, req, sizeof *req) != XTC_OK) {
		(void)xtc_proc_sleep(RPC_NS);
		return XTC_E_AGAIN;
	}
	deadline = xtc_clock_mono() + RPC_NS;
	for (;;) {
		left = deadline - xtc_clock_mono();
		if (left <= 0 || recv_msg(rep, left) == XTC_E_AGAIN)
			return XTC_E_AGAIN;
		if (rep->rid == req->rid)
			return XTC_OK;
	}
}

/* ---- the queue ---------------------------------------------------- */

/* One journal write: the fsync's latency, after which the caller's
 * update is durable.  A kill during the sleep unwinds the queue before
 * the update. */
static void
journal_sync(void)
{
	int64_t ns = g_cfg.journal_ns;

	if (XTC_SIM_BUGGIFY("jq.queue.slow_journal"))
		ns *= 8;
	(void)xtc_proc_sleep(ns);
}

/* Start an incarnation: a new epoch, durable before any lease uses it,
 * then the job table from the journal, with every lease forgotten. */
static void
queue_recover(void)
{
	uint32_t epoch = g_journal.epoch + 1;
	int j;

#if XTC_DST_BUG(JQ_BUG_EPOCH)
	if (epoch > 1)
		epoch--; /* tokens start over: one token, two holders */
#endif
	journal_sync();
	g_journal.epoch = epoch;
	g_epoch = epoch;
	g_granted = 0;
	for (j = 0; j < g_cfg.n_jobs; j++) {
		g_q[j].state = g_journal.state[j];
		g_q[j].token = 0;
		g_q[j].deadline = 0;
	}
	g_stats.epochs++;
	(void)xtc_reg_del(g_reg, "jobq", XTC_PID_NONE);
	(void)xtc_reg_put(g_reg, "jobq", xtc_self(), NULL, 0);
	jq_trace("queue.epoch", 0, epoch);
}

static void
queue_submit(const struct jq_msg *m)
{
	struct qjob *q = &g_q[m->job];

	if (q->state != JS_NONE) {
		g_stats.dup_submits++;
		reply(m, M_ACK);
		return;
	}
#if XTC_DST_BUG(JQ_BUG_EARLYACK)
	q->state = JS_READY;
	reply(m, M_ACK);
	journal_sync();
	g_journal.state[m->job] = JS_READY;
#else
	journal_sync();
	g_journal.state[m->job] = JS_READY;
	q->state = JS_READY;
	reply(m, M_ACK);
#endif
	jq_trace("queue.submit", m->job, 0);
}

/* Grant the first ready job whose lease is free or has expired. */
static void
queue_lease(const struct jq_msg *m)
{
	int64_t now = xtc_clock_mono(), len = g_cfg.lease_ns;
	struct jq_msg r = *m;
	int j;

	for (j = 0; j < g_cfg.n_jobs; j++)
		if (g_q[j].state == JS_READY &&
		    (g_q[j].token == 0 || g_q[j].deadline <= now))
			break;
	if (j == g_cfg.n_jobs) {
		reply(m, M_NONE);
		return;
	}
	if (g_q[j].token != 0)
		g_stats.regrants++;
	if (XTC_SIM_BUGGIFY("jq.queue.short_lease"))
		len /= 8;
	g_q[j].token = (uint64_t)g_epoch << 32 | ++g_granted;
	g_q[j].deadline = now + len;
	r.type = M_GRANT;
	r.job = j;
	r.token = g_q[j].token;
	r.deadline = g_q[j].deadline;
	(void)xtc_send(m->from, &r, sizeof r);
	jq_trace("queue.grant", j, r.token);
}

/* Accept a completion only from the job's current lease.  A lease that
 * expired but was not granted again is still current: its holder's
 * write passed the fence. */
static void
queue_complete(const struct jq_msg *m)
{
	struct qjob *q = &g_q[m->job];

	if (q->state == JS_DONE) {
		reply(m,
		    g_journal.done_token[m->job] == m->token ? M_ACK : M_STALE);
		return;
	}
	if (q->state != JS_READY || q->token != m->token) {
		g_stats.stale_completes++;
		reply(m, M_STALE);
		jq_trace("queue.stale", m->job, m->token);
		return;
	}
	journal_sync();
	g_journal.done_token[m->job] = m->token;
	g_journal.state[m->job] = JS_DONE;
	q->state = JS_DONE;
	reply(m, M_ACK);
	jq_trace("queue.done", m->job, m->token);
}

static void
queue_main(void *arg)
{
	struct jq_msg m, r;

	(void)arg;
	queue_recover();
	for (;;) {
		if (recv_msg(&m, -1) != XTC_OK)
			continue;
		if (m.type != M_LEASE && (m.job < 0 || m.job >= g_cfg.n_jobs))
			continue;
		switch (m.type) {
		case M_SUBMIT:
			queue_submit(&m);
			break;
		case M_LEASE:
			queue_lease(&m);
			break;
		case M_COMPLETE:
			queue_complete(&m);
			break;
		case M_STATUS:
			r = m;
			r.type = M_STATE;
			r.status = g_q[m.job].state;
			(void)xtc_send(m.from, &r, sizeof r);
			break;
		default:
			break;
		}
	}
}

/* ---- the object store --------------------------------------------- */

/* Apply a write if its token is not below the job's fence, and log it
 * for the checks.  A repeated write with the same token is accepted
 * again: it is a retry of the same lease's write. */
static void
store_write(const struct jq_msg *m)
{
	int accept = m->token >= g_store.fence[m->job];

#if XTC_DST_BUG(JQ_BUG_NOFENCE)
	accept = 1;
#endif
	if (!accept) {
		g_stats.fenced++;
		reply(m, M_FENCED);
		jq_trace("store.fenced", m->job, m->token);
		return;
	}
	g_store.fence[m->job] = m->token;
	if (g_store.n_log < LOG_MAX) {
		g_store.log[g_store.n_log].job = m->job;
		g_store.log[g_store.n_log].worker = m->worker;
		g_store.log[g_store.n_log].token = m->token;
		g_store.n_log++;
	} else {
		g_store.overflow = 1;
	}
	reply(m, M_ACK);
	jq_trace("store.write", m->job, m->token);
}

static void
store_main(void *arg)
{
	struct jq_msg m;

	(void)arg;
	xtc_atomic_pid_store(&g_store_pid, xtc_self());
	(void)xtc_reg_put(g_reg, "store", xtc_self(), NULL, 0);
	for (;;) {
		if (recv_msg(&m, -1) != XTC_OK)
			continue;
		if (m.type == M_WRITE && m.job >= 0 && m.job < g_cfg.n_jobs)
			store_write(&m);
	}
}

/* ---- the workers -------------------------------------------------- */

/* Write the job's result to the store under the lease's token.  M_ACK,
 * M_FENCED, or 0 when the store could not be reached. */
static int
write_result(int id, const struct jq_msg *g)
{
	struct jq_msg req, rep;
	int i;

	for (i = 0; i < ATTEMPTS; i++) {
		memset(&req, 0, sizeof req);
		req.type = M_WRITE;
		req.job = g->job;
		req.token = g->token;
		req.worker = id;
		if (rpc("store", &req, &rep) == XTC_OK &&
		    (rep.type == M_ACK || rep.type == M_FENCED))
			return rep.type;
	}
	return 0;
}

/* Report the job complete, retrying on silence.  The queue answers a
 * repeat with the same answer, so a duplicate is harmless. */
static void
complete(const struct jq_msg *g)
{
	struct jq_msg req, rep;
	int i, copies = 1;

	if (XTC_SIM_BUGGIFY("jq.worker.dup_complete"))
		copies = 2;
	for (i = 0; i < ATTEMPTS * copies; i++) {
		memset(&req, 0, sizeof req);
		req.type = M_COMPLETE;
		req.job = g->job;
		req.token = g->token;
		if (rpc("jobq", &req, &rep) == XTC_OK && --copies == 0)
			return;
	}
}

static void
run_job(int id, const struct jq_msg *g)
{
	(void)xtc_proc_sleep(1 * MS + (int64_t)xtc_sim_rand(WORK_NS));
	if (XTC_SIM_BUGGIFY("jq.worker.abandon") && xtc_sim_buggify_fault(200))
		return; /* the lease expires and the job is granted again */
	if (xtc_clock_mono() >= g->deadline) {
		jq_trace("worker.expired", g->job, g->token);
		return;
	}
	/*
	 * The lease was valid a moment ago.  A pause here (a long GC, a
	 * swapped-out page, a stalled VM) can outlive it, and the write
	 * below then lands after the job's next holder wrote: only the
	 * fence stops it.
	 */
	if (XTC_SIM_BUGGIFY("jq.worker.pause_before_write") &&
	    xtc_sim_buggify_fault(300))
		(void)xtc_proc_sleep(3 * g_cfg.lease_ns);
	if (write_result(id, g) != M_ACK)
		return;
	complete(g);
}

static void
worker_main(void *arg)
{
	int id = (int)(intptr_t)arg;
	struct jq_msg req, rep;

	xtc_atomic_pid_store(&g_worker[id], xtc_self());
	for (;;) {
		memset(&req, 0, sizeof req);
		req.type = M_LEASE;
		if (rpc("jobq", &req, &rep) != XTC_OK)
			continue;
		if (rep.type == M_GRANT)
			run_job(id, &rep);
		else
			(void)xtc_proc_sleep(POLL_NS);
	}
}

/* ---- the producer ------------------------------------------------- */

/* Submit job j until the queue acknowledges it or the deadline passes. */
static void
submit(int j, int64_t give_up)
{
	struct jq_msg req, rep;
	int copies = XTC_SIM_BUGGIFY("jq.producer.dup_submit") ? 2 : 1;

	while (copies > 0 && xtc_clock_mono() < give_up) {
		memset(&req, 0, sizeof req);
		req.type = M_SUBMIT;
		req.job = j;
		if (rpc("jobq", &req, &rep) == XTC_OK && rep.type == M_ACK) {
			g_acked[j] = 1;
			copies--;
		}
	}
}

/* Ask about each acknowledged job until all are done.  A job the queue
 * says it never heard of was acknowledged and then lost. */
static void
wait_done(int64_t give_up)
{
	struct jq_msg req, rep;
	int j, open;

	for (;;) {
		open = 0;
		for (j = 0; j < g_cfg.n_jobs; j++) {
			if (!g_acked[j] || g_lost[j] || g_seen[j])
				continue;
			memset(&req, 0, sizeof req);
			req.type = M_STATUS;
			req.job = j;
			if (rpc("jobq", &req, &rep) == XTC_OK &&
			    rep.type == M_STATE && rep.status != JS_READY) {
				if (rep.status == JS_DONE)
					g_seen[j] = 1;
				else
					g_lost[j] = 1;
				continue;
			}
			open++;
		}
		if (open == 0)
			return;
		if (xtc_clock_mono() >= give_up) {
			g_gave_up = 1;
			return;
		}
		(void)xtc_proc_sleep(5 * MS);
	}
}

static void
producer_main(void *arg)
{
	int64_t give_up = xtc_clock_mono() + g_cfg.give_up_ns;
	int i, j;

	(void)arg;
	for (j = 0; j < g_cfg.n_jobs; j++)
		submit(j, give_up);
	wait_done(give_up);
	jq_trace("producer.done", g_gave_up, 0);
	xtc_atomic_store_i32(&g_finished, 1, xtc_mo_release);

	/* Shut the service down: stop the store, then each supervisor,
	 * which stops its children.  A kill, not a message: a message can
	 * be lost, and a store left running would keep the run alive. */
	(void)xtc_exit_pid(xtc_atomic_pid_load(&g_store_pid), 0);
	for (i = 0; i < 3; i++)
		(void)xtc_sup_stop(g_sup[i]);
}

/* ---- set up, tear down, check -------------------------------------- */

static void
reset(const struct jq_config *cfg)
{
	int i;

	g_cfg = *cfg;
	memset(&g_journal, 0, sizeof g_journal);
	memset(g_q, 0, sizeof g_q);
	memset(&g_store, 0, sizeof g_store);
	memset(&g_stats, 0, sizeof g_stats);
	memset(g_acked, 0, sizeof g_acked);
	memset(g_lost, 0, sizeof g_lost);
	memset(g_seen, 0, sizeof g_seen);
	g_epoch = g_granted = 0;
	g_gave_up = 0;
	g_trace = 0;
	xtc_atomic_store_u64(&g_next_rid, 1, xtc_mo_relaxed);
	xtc_atomic_store_i32(&g_finished, 0, xtc_mo_relaxed);
	for (i = 0; i < JQ_WORKERS; i++)
		xtc_atomic_pid_store(&g_worker[i], XTC_PID_NONE);
	xtc_atomic_pid_store(&g_store_pid, XTC_PID_NONE);
	for (i = 0; i < 3; i++)
		g_sup[i] = NULL;
}

/* A supervisor on `loop` for children placed on that same loop. */
static int
start_sup(xtc_exec_t *e, int loop, xtc_child_spec_t *kids, int n,
    xtc_supervisor_t **out)
{
	xtc_sup_opts_t opts = XTC_SUP_OPTS_INIT;

	opts.max_restarts = 1000; /* the harness kills often, on purpose */
	opts.period_ns = 1000 * MS;
	return xtc_sup_start(xtc_exec_loop(e, loop), &opts, kids, n, out);
}

int
jq_start(xtc_exec_t *e, const struct jq_config *cfg)
{
	xtc_child_spec_t q = XTC_CHILD_SPEC_INIT;
	xtc_child_spec_t w[JQ_WORKERS];
	int i, rc;

	if (e == NULL || cfg == NULL || cfg->n_jobs < 1 ||
	    cfg->n_jobs > JQ_JOBS_MAX || xtc_exec_n_loops(e) != JQ_LOOPS)
		return XTC_E_INVAL;
	reset(cfg);
	if ((rc = xtc_reg_create(&g_reg)) != XTC_OK)
		return rc;
	q.name = "queue";
	q.fn = queue_main;
	q.policy = XTC_RESTART_PERMANENT;
	for (i = 0; i < JQ_WORKERS; i++) {
		w[i] = (xtc_child_spec_t)XTC_CHILD_SPEC_INIT;
		w[i].name = "worker";
		w[i].fn = worker_main;
		w[i].arg = (void *)(intptr_t)i;
		w[i].policy = XTC_RESTART_PERMANENT;
	}
	if ((rc = start_sup(e, 0, &q, 1, &g_sup[0])) != XTC_OK ||
	    (rc = start_sup(e, 2, &w[0], 2, &g_sup[1])) != XTC_OK ||
	    (rc = start_sup(e, 3, &w[2], 2, &g_sup[2])) != XTC_OK ||
	    (rc = xtc_proc_spawn(xtc_exec_loop(e, 1), store_main, NULL, NULL,
	         NULL)) != XTC_OK ||
	    (rc = xtc_proc_spawn(xtc_exec_loop(e, 1), producer_main, NULL, NULL,
	         NULL)) != XTC_OK)
		return rc;
	return XTC_OK;
}

void
jq_finish(void)
{
	int i;

	for (i = 0; i < 3; i++)
		if (g_sup[i] != NULL)
			(void)xtc_sup_join(g_sup[i], 0);
	xtc_reg_destroy(g_reg);
	g_reg = NULL;
}

/* Two writes for one job: a stale holder after a newer one (FENCE), or
 * two workers under one lease (DOUBLE-EXEC).  1 and a message, or 0. */
static int
check_writes(int j, char *why, size_t len)
{
	int i, w = -1;
	uint64_t last = 0;

	for (i = 0; i < g_store.n_log; i++) {
		if (g_store.log[i].job != j)
			continue;
		if (g_store.log[i].token < last) {
			snprintf(why, len,
			    "FENCE: job %d: a write under lease "
			    "%llx landed after one under the newer lease %llx",
			    j, (unsigned long long)g_store.log[i].token,
			    (unsigned long long)last);
			return 1;
		}
		if (g_store.log[i].token == last &&
		    g_store.log[i].worker != w) {
			snprintf(why, len,
			    "DOUBLE-EXEC: job %d: workers %d and "
			    "%d both ran it under the one lease %llx",
			    j, w, g_store.log[i].worker,
			    (unsigned long long)last);
			return 1;
		}
		last = g_store.log[i].token;
		w = g_store.log[i].worker;
	}
	return 0;
}

int
jq_check(char *why, size_t len)
{
	int j;

	why[0] = '\0';
	if (g_store.overflow) {
		snprintf(why, len, "the store's write log overflowed");
		return XTC_E_INTERNAL;
	}
	for (j = 0; j < g_cfg.n_jobs; j++) {
		if (g_acked[j] &&
		    (g_lost[j] || g_journal.state[j] == JS_NONE)) {
			snprintf(why, len,
			    "ACKED-LOST: job %d was acknowledged "
			    "and then lost",
			    j);
			return XTC_E_INTERNAL;
		}
		if (check_writes(j, why, len))
			return XTC_E_INTERNAL;
		if (g_journal.state[j] == JS_DONE &&
		    g_store.fence[j] != g_journal.done_token[j]) {
			snprintf(why, len,
			    "RESULT: job %d completed under lease "
			    "%llx, but the stored result is from lease %llx",
			    j, (unsigned long long)g_journal.done_token[j],
			    (unsigned long long)g_store.fence[j]);
			return XTC_E_INTERNAL;
		}
	}
	for (j = 0; j < g_cfg.n_jobs; j++)
		if (!g_acked[j] || g_journal.state[j] != JS_DONE) {
			snprintf(why, len, "LIVENESS: job %d is not done (%s)",
			    j,
			    g_acked[j] ? "acknowledged" : "never acknowledged");
			return XTC_E_INTERNAL;
		}
	if (g_gave_up || !jq_finished()) {
		snprintf(why, len, "LIVENESS: the producer did not finish");
		return XTC_E_INTERNAL;
	}
	return XTC_OK;
}
