/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/01_hello_async.c -- minimal coroutine: spawn an async
 * task, await its return value, tear everything down.
 *
 * Build:
 *   cc -I../src/inc 01_hello_async.c ../build_unix/libxtc.a -lpthread -o 01_hello
 *
 * This is the first file most people copy, so it models the whole
 * lifecycle, including the failure paths: every call that can fail has
 * its return code checked, and once the loop exists every exit goes
 * through xtc_loop_destroy (which also frees any task still on it, and
 * cannot fail).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_async.h"

static intptr_t
hello(void *arg)
{
	int n = (int)(intptr_t)arg;
	printf("hello from coroutine, computing %d * %d\n", n, n);
	xtc_yield();           /* round-trip through the loop */
	return n * n;
}

int
main(int argc, char **argv)
{
	xtc_loop_t *loop;
	xtc_task_t *task;
	intptr_t result = 0;
	int rc, status = 1;

	if (argc > 1 && strcmp(argv[1], "--help") == 0) {
		printf("usage: %s\n"
		    "Spawns one coroutine that computes 7 * 7, awaits it, and "
		    "prints the result.\n", argv[0]);
		return 0;
	}

	if ((rc = xtc_loop_create(&loop)) != XTC_OK) {
		fprintf(stderr, "xtc_loop_create: %s\n", xtc_strerror(rc));
		return 1;
	}
	if ((rc = xtc_coro_spawn(loop, hello, (void *)(intptr_t)7, &task)) != XTC_OK) {
		fprintf(stderr, "xtc_coro_spawn: %s\n", xtc_strerror(rc));
		goto out;
	}
	if ((rc = xtc_loop_run(loop)) != XTC_OK) {
		fprintf(stderr, "xtc_loop_run: %s\n", xtc_strerror(rc));
		goto out;
	}
	if ((rc = xtc_coro_join(task, &result)) != XTC_OK) {
		fprintf(stderr, "xtc_coro_join: %s\n", xtc_strerror(rc));
		goto out;
	}
	printf("result = %lld\n", (long long)result);
	status = result == 49 ? 0 : 1;

out:
	/* One teardown for every path: frees the loop, its I/O backend,
	 * and the task (a task belongs to its loop; there is no separate
	 * task-free call). */
	xtc_loop_destroy(loop);
	return status;
}
