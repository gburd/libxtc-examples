/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/20_jobq/main.c -- run the job queue on real threads.
 *
 * The same service code the DST harness (dst.c) runs under the
 * simulator, here on four OS threads with the real clock and no faults:
 * the producer submits the jobs, four workers run them, and the program
 * checks the same invariants the harness checks.  Exit 0 iff they hold.
 *
 * Uses only the public xtc_* API.
 */

#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_exec.h"

#include "jobq.h"

int
main(int argc, char **argv)
{
	struct jq_config cfg;
	const struct jq_stats *st;
	xtc_exec_t *e = NULL;
	char why[256];
	int rc;

	if (argc > 1) {
		printf("usage: %s\n"
		       "  Runs the job queue on %d loops with real threads and "
		       "checks it.\n"
		       "  The DST harness is jobq_dst (jobq_dst --help).\n",
		    argv[0], JQ_LOOPS);
		return strcmp(argv[1], "--help") == 0 ? 0 : 2;
	}
	jq_config_default(&cfg);
	if ((rc = xtc_exec_create(&(xtc_exec_opts_t){
	    .size = sizeof(xtc_exec_opts_t), .n_loops = JQ_LOOPS },
	    &e)) != XTC_OK ||
	    (rc = jq_start(e, &cfg)) != XTC_OK) {
		fprintf(stderr, "jobq: start failed: %s\n", xtc_strerror(rc));
		return 1;
	}
	rc = xtc_exec_run(e);
	jq_finish();
	xtc_exec_destroy(e);
	if (rc != XTC_OK) {
		fprintf(stderr, "jobq: run failed: %s\n", xtc_strerror(rc));
		return 1;
	}
	if (jq_check(why, sizeof why) != XTC_OK) {
		printf("jobq: FAIL: %s\n", why);
		return 1;
	}
	st = jq_stats();
	printf("jobq: %d jobs done on %d workers; queue epochs %d, leases "
	       "granted again %d, stale writes fenced %d\n",
	    cfg.n_jobs, JQ_WORKERS, st->epochs, st->regrants, st->fenced);
	return 0;
}
