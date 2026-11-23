/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 05_rexis/test/rexis_killwrite.c
 *	An LD_PRELOAD shim that crashes rexis-server-xtc in the middle of a
 *	log write, for test_rexis_restart's mid-command cases.
 *
 *	With REXIS_KILL_WRITE=N in the environment, the server's Nth
 *	pwritev(2) writes the first half of its bytes and then the process
 *	SIGKILLs itself: the log ends in a torn record, exactly what a crash
 *	during the write leaves.  Every other pwritev runs unchanged.  The
 *	log (bitcask.c) is the only caller of pwritev in the server.
 *
 *	Test code only: it is never linked into the server.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

typedef ssize_t (*pwritev_fn)(int, const struct iovec *, int, off_t);

static long kill_at = -1;       /* REXIS_KILL_WRITE, read once */
static long n_calls;

/*
 * Write the first half of iov's bytes with one real pwritev, then die.
 * The iovec copy keeps the caller's array untouched.
 */
static void
torn_write_and_die(pwritev_fn real, int fd, const struct iovec *iov,
    int iovcnt, off_t off)
{
	struct iovec part[64];
	size_t total = 0, left;
	int i, n = 0;

	for (i = 0; i < iovcnt; i++)
		total += iov[i].iov_len;
	left = total / 2;
	for (i = 0; i < iovcnt && i < 64 && left > 0; i++) {
		part[n] = iov[i];
		if (part[n].iov_len > left)
			part[n].iov_len = left;
		left -= part[n].iov_len;
		n++;
	}
	if (n > 0)
		(void)real(fd, part, n, off);
	(void)kill(getpid(), SIGKILL);
	for (;;)
		pause();
}

ssize_t
pwritev(int fd, const struct iovec *iov, int iovcnt, off_t off)
{
	static pwritev_fn real;
	const char *s;

	if (real == NULL)
		real = (pwritev_fn)(uintptr_t)dlsym(RTLD_NEXT, "pwritev");
	if (kill_at < 0) {
		s = getenv("REXIS_KILL_WRITE");
		kill_at = s != NULL ? atol(s) : 0;
	}
	if (kill_at > 0 && ++n_calls == kill_at)
		torn_write_and_die(real, fd, iov, iovcnt, off);
	return real(fd, iov, iovcnt, off);
}
