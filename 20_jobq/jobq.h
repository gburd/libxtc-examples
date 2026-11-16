/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/20_jobq/jobq.h -- a job queue with leases, retries and
 * fencing tokens, built to be tested by deterministic simulation.
 *
 * The service (jobq.c) is ordinary libxtc code: procs on four loops that
 * talk only by messages.  main.c runs it on real threads; dst.c runs the
 * same code under the simulator with faults.  Both use only this header
 * and the public xtc_* API.
 */

#ifndef JOBQ_H
#define JOBQ_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"

#define JQ_LOOPS 4    /* loop 0 queue, 1 store + producer, 2-3 workers */
#define JQ_WORKERS 4  /* two per worker loop */
#define JQ_JOBS_MAX 64

/* What a run does.  jq_config_default fills in the values main.c uses. */
struct jq_config {
	int n_jobs;          /* jobs the producer submits, <= JQ_JOBS_MAX */
	int64_t lease_ns;    /* how long a lease lasts */
	int64_t journal_ns;  /* one durable journal write (an fsync) */
	int64_t give_up_ns;  /* the producer stops waiting after this */
};

/* Counters a run leaves behind, for the harness's coverage report. */
struct jq_stats {
	int epochs;          /* queue incarnations (restarts + 1) */
	int regrants;        /* expired leases granted again */
	int fenced;          /* stale writes the store refused */
	int stale_completes; /* completions the queue refused */
	int dup_submits;     /* submits of a job the queue already had */
};

void jq_config_default(struct jq_config *cfg);

/* Reset all service state and spawn the service on e's loops.  Call
 * before xtc_exec_run or xtc_sim_exec_run.  Returns XTC_OK or an
 * XTC_E_* code. */
int jq_start(xtc_exec_t *e, const struct jq_config *cfg);

/* After the run: free the supervisors.  Call before xtc_exec_fini. */
void jq_finish(void);

/* The safety and liveness checks.  XTC_OK if all hold; otherwise
 * XTC_E_INTERNAL with the first violation described in why. */
int jq_check(char *why, size_t len);

/* 1 once the producer has finished (all done, or given up). */
int jq_finished(void);

/* The queue's and worker i's current pids (none while restarting). */
xtc_pid_t jq_queue_pid(void);
xtc_pid_t jq_worker_pid(int i);

/* A hash of every state change in order, with its virtual time.  Kept
 * only under the simulator; two runs of one seed must agree. */
uint64_t jq_trace_hash(void);

const struct jq_stats *jq_stats(void);

/* Record one state change in the trace hash (and print it when verbose
 * is on).  The service calls it; the harness may too. */
void jq_trace(const char *ev, int a, uint64_t b);
void jq_set_verbose(int on);

#endif /* JOBQ_H */
