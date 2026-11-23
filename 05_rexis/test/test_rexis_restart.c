/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 05_rexis/test/test_rexis_restart.c
 *	--persist across a real server restart.  Each test starts
 *	rexis-server-xtc with --persist=DIR, runs commands of one class
 *	(counters, TTLs, FLUSHDB, lists, hashes, deletes), then stops the
 *	server and starts a new one on the same directory, and checks that
 *	the keyspace came back exactly as acknowledged: every probe (a read
 *	command) must answer byte for byte what it answered before the
 *	restart.
 *
 *	Each test runs twice: "term" stops the server with SIGTERM (a clean
 *	shutdown), "kill" with SIGKILL between two commands (a crash).  The
 *	log is written with pwrite and no fsync, so a crash of the process
 *	keeps every acknowledged write (the page cache has it); a power loss
 *	is not tested and not promised (05_rexis/README.md).
 *
 *	The log directory is under $TMPDIR (else /tmp) and is removed at the
 *	end of each test.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "munit.h"

#include "rexis_harness.h"

/* One test's server and log directory. */
typedef struct rt {
	rexis_srv_t srv;
	int         fd;          /* client connection to srv */
	int         crash;       /* restart with SIGKILL, not SIGTERM */
	char        dir[256];
	char        arg[300];    /* --persist=DIR */
} rt_t;

static char *stop_modes[] = { "term", "kill", NULL };
static MunitParameterEnum params[] = {
	{ "stop", stop_modes },
	{ NULL, NULL }
};

/* ----- RESP replies ----- */

/* Read one CRLF-terminated line (CRLF included) into buf at *len.
 * Returns 0, or -1 on EOF / timeout / overflow. */
static int
read_line(int fd, char *buf, size_t cap, size_t *len)
{
	while (*len + 1 < cap) {
		if (recv(fd, buf + *len, 1, 0) != 1)
			return -1;
		(*len)++;
		if (*len >= 2 && buf[*len - 2] == '\r' && buf[*len - 1] == '\n')
			return 0;
	}
	return -1;
}

/* Read one whole RESP reply (nested arrays included) into buf, NUL
 * terminated.  Returns 0, or -1. */
static int
read_reply(int fd, char *buf, size_t cap, size_t *len)
{
	size_t start = *len;
	long n, i;

	if (read_line(fd, buf, cap, len) != 0)
		return -1;
	buf[*len] = '\0';
	n = strtol(buf + start + 1, NULL, 10);
	if (buf[start] == '$' && n >= 0) {
		if (*len + (size_t)n + 2 + 1 > cap ||
		    rexis_read_n(fd, buf + *len, (size_t)n + 2, 3000) !=
		    (size_t)n + 2)
			return -1;
		*len += (size_t)n + 2;
	} else if (buf[start] == '*') {
		for (i = 0; i < n; i++)
			if (read_reply(fd, buf, cap, len) != 0)
				return -1;
	}
	buf[*len] = '\0';
	return 0;
}

/* Send a command of argc words (NULL-terminated array) and read its
 * reply into out (cap bytes).  Fails the test on an I/O error. */
static void
rt_cmdv(rt_t *t, const char *const *argv, char *out, size_t cap)
{
	char cmd[4096];
	size_t used = 0, len = 0;
	int argc = 0, i;

	while (argv[argc] != NULL)
		argc++;
	used = (size_t)snprintf(cmd, sizeof cmd, "*%d\r\n", argc);
	for (i = 0; i < argc; i++)
		used += (size_t)snprintf(cmd + used, sizeof cmd - used,
		    "$%zu\r\n%s\r\n", strlen(argv[i]), argv[i]);
	munit_assert_size(used, <, sizeof cmd);
	munit_assert_int(rexis_send_all(t->fd, cmd, used), ==, 0);
	munit_assert_int(read_reply(t->fd, out, cap, &len), ==, 0);
}

/* Run a command and require its exact reply. */
#define RT_EXPECT(t, want, ...) do {                                    \
	const char *const av_[] = { __VA_ARGS__, NULL };                \
	char got_[4096];                                                 \
	rt_cmdv((t), av_, got_, sizeof got_);                            \
	munit_assert_string_equal(got_, (want));                         \
} while (0)

/* Run a command and return its reply in a static buffer. */
static const char *
rt_reply(rt_t *t, const char *const *argv)
{
	static char got[4096];
	rt_cmdv(t, argv, got, sizeof got);
	return got;
}

/* ----- Server lifecycle ----- */

static void
rt_connect(rt_t *t)
{
	t->fd = rexis_connect(&t->srv, 3000);
	munit_assert_int(t->fd, >=, 0);
}

static void
rt_start(rt_t *t)
{
	const char *const extra[] = { t->arg };

	munit_assert_int(rexis_start(&t->srv, extra, 1), ==, 0);
	rt_connect(t);
}

/* Stop the server: SIGTERM and require a clean exit, or SIGKILL. */
static void
rt_stop(rt_t *t)
{
	close(t->fd);
	t->fd = -1;
	if (!t->crash) {
		munit_assert_int(rexis_stop(&t->srv), ==, 0);
		return;
	}
	kill(t->srv.pid, SIGKILL);
	munit_assert_int(waitpid(t->srv.pid, NULL, 0), ==, t->srv.pid);
	t->srv.pid = -1;
}

static void
rt_restart(rt_t *t)
{
	rt_stop(t);
	rt_start(t);
}

static void *
rt_setup(const MunitParameter p[], void *user)
{
	static int seq;
	const char *base = getenv("TMPDIR");
	rt_t *t = calloc(1, sizeof *t);
	(void)user;

	munit_assert_not_null(t);
	t->crash = strcmp(munit_parameters_get(p, "stop"), "kill") == 0;
	snprintf(t->dir, sizeof t->dir, "%s/rexis-restart-%d-%d",
	    base != NULL && *base != '\0' ? base : "/tmp", (int)getpid(),
	    ++seq);
	munit_assert_int(mkdir(t->dir, 0700), ==, 0);
	snprintf(t->arg, sizeof t->arg, "--persist=%s", t->dir);
	rt_start(t);
	return t;
}

static void
rt_teardown(void *fixture)
{
	rt_t *t = fixture;
	char path[512];

	if (t->fd >= 0)
		close(t->fd);
	munit_assert_int(rexis_stop(&t->srv), ==, 0);
	snprintf(path, sizeof path, "%s/bitcask.v2", t->dir);
	(void)unlink(path);
	(void)rmdir(t->dir);
	free(t);
}

/*
 * Run each probe (a NULL-terminated list of read commands, each a
 * NULL-terminated word list), restart, run them again, and require the
 * same replies.  This is the "exactly as acknowledged" check.
 */
static void
rt_restart_same(rt_t *t, const char *const *const *probes)
{
	static char before[16][4096];
	int i;

	for (i = 0; probes[i] != NULL; i++) {
		munit_assert_int(i, <, 16);
		snprintf(before[i], sizeof before[i], "%s",
		    rt_reply(t, probes[i]));
	}
	rt_restart(t);
	for (i = 0; probes[i] != NULL; i++)
		munit_assert_string_equal(rt_reply(t, probes[i]), before[i]);
}

/* Seconds left on key, from TTL. */
static long
rt_ttl(rt_t *t, const char *key)
{
	const char *const av[] = { "TTL", key, NULL };
	const char *r = rt_reply(t, av);

	munit_assert_char(r[0], ==, ':');
	return strtol(r + 1, NULL, 10);
}

/* ----- The tests ----- */

/* INCR/DECR/INCRBY/DECRBY log the resulting value. */
static MunitResult
test_counters(const MunitParameter p[], void *f)
{
	rt_t *t = f;
	static const char *const a[] = { "GET", "a", NULL };
	static const char *const c[] = { "GET", "c", NULL };
	static const char *const n[] = { "GET", "neg", NULL };
	static const char *const *const probes[] = { a, c, n, NULL };
	(void)p;

	RT_EXPECT(t, "+OK\r\n", "SET", "a", "1");
	RT_EXPECT(t, ":2\r\n", "INCR", "a");
	RT_EXPECT(t, ":3\r\n", "INCR", "a");
	rt_restart(t);                    /* a crash between INCRs */
	RT_EXPECT(t, "$1\r\n3\r\n", "GET", "a");
	RT_EXPECT(t, ":13\r\n", "INCRBY", "a", "10");
	RT_EXPECT(t, ":12\r\n", "DECR", "a");
	RT_EXPECT(t, ":10\r\n", "DECRBY", "a", "2");
	RT_EXPECT(t, ":1\r\n", "INCR", "c");          /* a new key */
	RT_EXPECT(t, ":-5\r\n", "DECRBY", "neg", "5");
	rt_restart_same(t, probes);
	RT_EXPECT(t, "$2\r\n10\r\n", "GET", "a");
	RT_EXPECT(t, ":11\r\n", "INCR", "a");     /* counts on from there */
	return MUNIT_OK;
}

/* SET EX/PX and EXPIRE log an absolute expiry; a plain SET clears it;
 * INCR keeps it; a key that expired while the server was down stays
 * gone. */
static MunitResult
test_ttl(const MunitParameter p[], void *f)
{
	rt_t *t = f;
	static const char *const g1[] = { "GET", "ex", NULL };
	static const char *const g2[] = { "GET", "k", NULL };
	static const char *const g3[] = { "GET", "cnt", NULL };
	static const char *const t4[] = { "TTL", "plain", NULL };
	static const char *const *const probes[] = { g1, g2, g3, t4, NULL };
	(void)p;

	RT_EXPECT(t, "+OK\r\n", "SET", "ex", "v", "EX", "100");
	RT_EXPECT(t, "+OK\r\n", "SET", "k", "old");
	rt_restart(t);
	/* Over an older logged value: the new one, with its TTL, wins. */
	RT_EXPECT(t, "+OK\r\n", "SET", "k", "new", "PX", "100000");
	RT_EXPECT(t, "+OK\r\n", "SET", "cnt", "5", "EX", "100");
	RT_EXPECT(t, ":6\r\n", "INCR", "cnt");
	RT_EXPECT(t, "+OK\r\n", "SET", "plain", "v", "EX", "100");
	RT_EXPECT(t, "+OK\r\n", "SET", "plain", "v");   /* clears the TTL */
	RT_EXPECT(t, "+OK\r\n", "SET", "lst", "x");
	RT_EXPECT(t, ":1\r\n", "EXPIRE", "lst", "100");
	RT_EXPECT(t, "+OK\r\n", "SET", "short", "v", "PX", "400");
	RT_EXPECT(t, ":1\r\n", "EXISTS", "short");
	rt_stop(t);
	usleep(700 * 1000);               /* "short" expires while down */
	rt_start(t);
	RT_EXPECT(t, ":0\r\n", "EXISTS", "short");
	RT_EXPECT(t, ":5\r\n", "DBSIZE");  /* ex k cnt plain lst */
	RT_EXPECT(t, "$3\r\nnew\r\n", "GET", "k");
	RT_EXPECT(t, "$1\r\n6\r\n", "GET", "cnt");
	munit_assert_long(rt_ttl(t, "ex"), >=, 90);
	munit_assert_long(rt_ttl(t, "ex"), <=, 100);
	munit_assert_long(rt_ttl(t, "k"), >=, 90);
	munit_assert_long(rt_ttl(t, "cnt"), >=, 90);
	munit_assert_long(rt_ttl(t, "lst"), >=, 90);
	munit_assert_long(rt_ttl(t, "plain"), ==, -1);

	/* A replayed TTL still runs out while the server serves. */
	RT_EXPECT(t, "+OK\r\n", "SET", "soon", "v", "PX", "1200");
	rt_restart(t);
	RT_EXPECT(t, ":1\r\n", "EXISTS", "soon");
	usleep(1500 * 1000);
	RT_EXPECT(t, ":0\r\n", "EXISTS", "soon");
	rt_restart_same(t, probes);
	return MUNIT_OK;
}

/*
 * An out-of-range TTL is refused, as Redis refuses it, and changes
 * nothing; EXPIRE of zero or less deletes the key.  Memory and the log
 * agree on every one of them across a restart.
 */
static MunitResult
test_ttl_range(const MunitParameter p[], void *f)
{
	rt_t *t = f;
	static const char *const g1[] = { "GET", "neg", NULL };
	static const char *const g2[] = { "GET", "big", NULL };
	static const char *const t2[] = { "TTL", "big", NULL };
	static const char *const g3[] = { "GET", "z", NULL };
	static const char *const e4[] = { "EXISTS", "far", NULL };
	static const char *const *const probes[] = { g1, g2, t2, g3, e4,
	    NULL };
	(void)p;

	RT_EXPECT(t, "-ERR invalid expire time in 'set' command\r\n",
	    "SET", "neg", "v", "EX", "-1000000000");
	RT_EXPECT(t, "-ERR invalid expire time in 'set' command\r\n",
	    "SET", "neg", "v", "PX", "0");
	RT_EXPECT(t, "-ERR invalid expire time in 'set' command\r\n",
	    "SET", "far", "v", "EX", "9223372036");
	RT_EXPECT(t, "-ERR value is not an integer or out of range\r\n",
	    "SET", "neg", "v", "EX", "12abc");
	RT_EXPECT(t, "-ERR value is not an integer or out of range\r\n",
	    "SET", "neg", "v", "EX", "99999999999999999999");
	RT_EXPECT(t, ":0\r\n", "EXISTS", "neg");

	RT_EXPECT(t, "+OK\r\n", "SET", "big", "v");
	/* 253 years: past what int64_t wall-clock ns can hold. */
	RT_EXPECT(t, "-ERR invalid expire time in 'expire' command\r\n",
	    "EXPIRE", "big", "8000000000");
	RT_EXPECT(t, ":-1\r\n", "TTL", "big");
	RT_EXPECT(t, ":1\r\n", "EXPIRE", "big", "7000000000");  /* 221 y */
	munit_assert_long(rt_ttl(t, "big"), >, 6999999000L);

	RT_EXPECT(t, "+OK\r\n", "SET", "z", "v");
	RT_EXPECT(t, ":1\r\n", "EXPIRE", "z", "-5");    /* deletes */
	RT_EXPECT(t, ":0\r\n", "EXISTS", "z");
	RT_EXPECT(t, ":0\r\n", "EXPIRE", "z", "0");
	rt_restart_same(t, probes);
	RT_EXPECT(t, "$1\r\nv\r\n", "GET", "big");
	return MUNIT_OK;
}

/* FLUSHDB logs a flush record: the keys before it stay gone. */
static MunitResult
test_flush(const MunitParameter p[], void *f)
{
	rt_t *t = f;
	(void)p;

	RT_EXPECT(t, "+OK\r\n", "SET", "f1", "a");
	RT_EXPECT(t, ":1\r\n", "RPUSH", "fl", "x");
	RT_EXPECT(t, ":1\r\n", "HSET", "fh", "k", "v");
	RT_EXPECT(t, "+OK\r\n", "SET", "f2", "b", "EX", "100");
	rt_restart(t);
	RT_EXPECT(t, ":4\r\n", "DBSIZE");
	RT_EXPECT(t, "+OK\r\n", "FLUSHDB");
	RT_EXPECT(t, "+OK\r\n", "SET", "after", "c");
	rt_restart(t);
	RT_EXPECT(t, ":1\r\n", "DBSIZE");
	RT_EXPECT(t, "$-1\r\n", "GET", "f1");
	RT_EXPECT(t, "$1\r\nc\r\n", "GET", "after");
	RT_EXPECT(t, "+OK\r\n", "FLUSHDB");
	rt_restart(t);
	RT_EXPECT(t, ":0\r\n", "DBSIZE");
	return MUNIT_OK;
}

/* LPUSH/RPUSH/LPOP/RPOP log the resulting list; emptying it deletes. */
static MunitResult
test_list(const MunitParameter p[], void *f)
{
	rt_t *t = f;
	static const char *const r[] = { "LRANGE", "l", "0", "-1", NULL };
	static const char *const e[] = { "EXISTS", "gone", NULL };
	static const char *const *const probes[] = { r, e, NULL };
	(void)p;

	RT_EXPECT(t, ":3\r\n", "RPUSH", "l", "a", "b", "c");
	RT_EXPECT(t, ":4\r\n", "LPUSH", "l", "z");
	rt_restart(t);
	RT_EXPECT(t, "*4\r\n$1\r\nz\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n",
	    "LRANGE", "l", "0", "-1");
	RT_EXPECT(t, "$1\r\nz\r\n", "LPOP", "l");
	RT_EXPECT(t, "$1\r\nc\r\n", "RPOP", "l");
	RT_EXPECT(t, ":4\r\n", "RPUSH", "l", "", "d");   /* an empty item */
	RT_EXPECT(t, ":1\r\n", "RPUSH", "gone", "x");
	RT_EXPECT(t, "$1\r\nx\r\n", "LPOP", "gone");
	rt_restart_same(t, probes);
	RT_EXPECT(t, "*4\r\n$1\r\na\r\n$1\r\nb\r\n$0\r\n\r\n$1\r\nd\r\n",
	    "LRANGE", "l", "0", "-1");
	RT_EXPECT(t, ":0\r\n", "EXISTS", "gone");
	RT_EXPECT(t, ":1\r\n", "DBSIZE");
	return MUNIT_OK;
}

/* HSET/HDEL log the resulting hash, field order included. */
static MunitResult
test_hash(const MunitParameter p[], void *f)
{
	rt_t *t = f;
	static const char *const g[] = { "HGETALL", "h", NULL };
	static const char *const n[] = { "HLEN", "h", NULL };
	static const char *const e[] = { "EXISTS", "gone", NULL };
	static const char *const *const probes[] = { g, n, e, NULL };
	(void)p;

	RT_EXPECT(t, ":2\r\n", "HSET", "h", "f1", "v1", "f2", "v2");
	rt_restart(t);
	RT_EXPECT(t, ":0\r\n", "HSET", "h", "f1", "V1");    /* replace */
	RT_EXPECT(t, ":1\r\n", "HDEL", "h", "f2");
	RT_EXPECT(t, ":2\r\n", "HSET", "h", "f3", "v3", "f4", "");
	RT_EXPECT(t, ":1\r\n", "HSET", "gone", "k", "v");
	RT_EXPECT(t, ":1\r\n", "HDEL", "gone", "k");
	rt_restart_same(t, probes);
	RT_EXPECT(t, "$2\r\nV1\r\n", "HGET", "h", "f1");
	RT_EXPECT(t, "$-1\r\n", "HGET", "h", "f2");
	RT_EXPECT(t, ":3\r\n", "HLEN", "h");
	RT_EXPECT(t, ":0\r\n", "EXISTS", "gone");
	return MUNIT_OK;
}

/* DEL of each type logs a tombstone; a re-created key replaces it. */
static MunitResult
test_del(const MunitParameter p[], void *f)
{
	rt_t *t = f;
	(void)p;

	/* An empty key with an empty value, as the log's first record: a
	 * put with no body bytes, read before any buffer exists. */
	RT_EXPECT(t, "+OK\r\n", "SET", "", "");
	rt_restart(t);
	RT_EXPECT(t, "$0\r\n\r\n", "GET", "");
	RT_EXPECT(t, "+OK\r\n", "SET", "s", "v");
	RT_EXPECT(t, ":1\r\n", "RPUSH", "l", "x");
	RT_EXPECT(t, ":1\r\n", "HSET", "h", "k", "v");
	RT_EXPECT(t, "+OK\r\n", "SET", "", "empty key");
	RT_EXPECT(t, "+OK\r\n", "SET", "ev", "");          /* empty value */
	rt_restart(t);
	RT_EXPECT(t, "$9\r\nempty key\r\n", "GET", "");
	RT_EXPECT(t, "$0\r\n\r\n", "GET", "ev");
	RT_EXPECT(t, ":5\r\n", "DEL", "s", "l", "h", "", "ev");

	RT_EXPECT(t, ":1\r\n", "RPUSH", "s", "now a list");
	rt_restart(t);
	RT_EXPECT(t, ":1\r\n", "DBSIZE");
	RT_EXPECT(t, "*1\r\n$10\r\nnow a list\r\n", "LRANGE", "s", "0", "-1");
	return MUNIT_OK;
}

/* ----- A crash in the middle of a command ----- */

/* Copy file `from` to `to` (both under the test's directory). */
static void
copy_file(const char *from, const char *to)
{
	char buf[65536];
	FILE *in, *out;
	size_t n;

	munit_assert_not_null(in = fopen(from, "rb"));
	munit_assert_not_null(out = fopen(to, "wb"));
	while ((n = fread(buf, 1, sizeof buf, in)) > 0)
		munit_assert_size(fwrite(buf, 1, n, out), ==, n);
	fclose(in);
	munit_assert_int(fclose(out), ==, 0);
}

/* Run the probes into replies[] (each is one reply, read back). */
static void
probe_all(rt_t *t, const char *const *const *probes, char (*replies)[4096])
{
	int i;

	for (i = 0; probes[i] != NULL; i++)
		snprintf(replies[i], 4096, "%s", rt_reply(t, probes[i]));
}

/*
 * Start a server whose kill_at'th log write is torn in half and kills it
 * (rexis_killwrite.so, LD_PRELOAD'd into the server only), send `cmd`,
 * and return 1 if it answered, 0 if it died first.  A server that
 * survives the command is stopped with SIGTERM.
 */
static int
run_with_torn_write(rt_t *t, int kill_at, const char *const *cmd)
{
	const char *so = getenv("REXIS_KILLWRITE_SO");
	char path[4096], at[16], out[4096];
	size_t len = 0, used = 0;
	int argc = 0, i, answered;
	char wire[4096];

	if (so == NULL || *so == '\0')
		so = "./rexis_killwrite.so";
	munit_assert_not_null(realpath(so, path));
	snprintf(at, sizeof at, "%d", kill_at);
	setenv("LD_PRELOAD", path, 1);
	setenv("REXIS_KILL_WRITE", at, 1);
	/* A gcc ASan server would refuse a preload ahead of libasan. */
	setenv("ASAN_OPTIONS", "verify_asan_link_order=0", 0);
	rt_start(t);
	unsetenv("LD_PRELOAD");
	unsetenv("REXIS_KILL_WRITE");

	while (cmd[argc] != NULL)
		argc++;
	used = (size_t)snprintf(wire, sizeof wire, "*%d\r\n", argc);
	for (i = 0; i < argc; i++)
		used += (size_t)snprintf(wire + used, sizeof wire - used,
		    "$%zu\r\n%s\r\n", strlen(cmd[i]), cmd[i]);
	munit_assert_size(used, <, sizeof wire);
	munit_assert_int(rexis_send_all(t->fd, wire, used), ==, 0);
	answered = read_reply(t->fd, out, sizeof out, &len) == 0;
	close(t->fd);
	t->fd = -1;
	if (answered) {
		munit_assert_int(rexis_stop(&t->srv), ==, 0);
	} else {
		munit_assert_int(waitpid(t->srv.pid, NULL, 0), ==, t->srv.pid);
		t->srv.pid = -1;
	}
	return answered;
}

/* 1 if every probe's reply in got equals the one in want. */
static int
same_replies(const char *const *const *probes, char (*got)[4096],
    char (*want)[4096])
{
	int i;

	for (i = 0; probes[i] != NULL; i++)
		if (strcmp(got[i], want[i]) != 0)
			return 0;
	return 1;
}

/*
 * One mid-command case, on the test's running server and empty log.
 * `setup` runs and its log is saved.  From that saved log each time,
 * `cmd` runs on a plain server to learn the keyspace after it, and then
 * on servers whose 1st, 2nd, ... log write is torn by a crash.  After
 * each crash the restarted keyspace (the probes) must be all of the
 * keyspace before the command or all of the one after it, never a part.
 * Returns with a server running on an empty log.
 */
static void
midcmd_case(rt_t *t, const char *const *const *setup,
    const char *const *cmd, const char *const *const *probes)
{
	static char before[16][4096], after[16][4096], got[16][4096];
	char log[512], saved[512];
	int i, k, died = 0;

	snprintf(log, sizeof log, "%s/bitcask.v2", t->dir);
	snprintf(saved, sizeof saved, "%s/setup.v2", t->dir);
	for (i = 0; setup[i] != NULL; i++)
		rt_reply(t, setup[i]);
	probe_all(t, probes, before);
	close(t->fd);
	t->fd = -1;
	munit_assert_int(rexis_stop(&t->srv), ==, 0);
	copy_file(log, saved);

	rt_start(t);
	rt_reply(t, cmd);
	probe_all(t, probes, after);
	close(t->fd);
	t->fd = -1;
	munit_assert_int(rexis_stop(&t->srv), ==, 0);

	for (k = 1; k <= 8; k++) {
		copy_file(saved, log);
		if (run_with_torn_write(t, k, cmd))
			break;            /* the command took < k writes */
		died++;
		rt_start(t);
		probe_all(t, probes, got);
		if (!same_replies(probes, got, before) &&
		    !same_replies(probes, got, after)) {
			for (i = 0; probes[i] != NULL; i++)
				fprintf(stderr, "  probe %d: got %s  before %s"
				    "  after %s\n", i, got[i], before[i],
				    after[i]);
			munit_errorf("a crash in log write %d of %s left a "
			    "part of the command", k, cmd[0]);
		}
		close(t->fd);
		t->fd = -1;
		munit_assert_int(rexis_stop(&t->srv), ==, 0);
	}
	munit_assert_int(died, >=, 1);
	munit_assert_int(unlink(saved), ==, 0);
	munit_assert_int(unlink(log), ==, 0);
	rt_start(t);
}

/* RPUSH, LPUSH, HSET, HDEL and multi-key DEL, each killed mid-write. */
static MunitResult
test_midcmd(const MunitParameter p[], void *f)
{
	rt_t *t = f;
	static const char *const *const none[] = { NULL };
	static const char *const pl[] = { "LRANGE", "l", "0", "-1", NULL };
	static const char *const pm[] = { "LRANGE", "m", "0", "-1", NULL };
	static const char *const ph[] = { "HGETALL", "h", NULL };
	static const char *const pa[] = { "GET", "a", NULL };
	static const char *const pb[] = { "GET", "b", NULL };
	static const char *const pc[] = { "GET", "c", NULL };
	static const char *const *const plist[] = { pl, pm, NULL };
	static const char *const *const phash[] = { ph, NULL };
	static const char *const *const pdel[] = { pa, pb, pc, pl, ph, NULL };
	static const char *const rpush[] = { "RPUSH", "l", "a", "b", "c",
	    "d", "e", NULL };
	static const char *const lpush[] = { "LPUSH", "m", "a", "b", "c",
	    "d", "e", NULL };
	static const char *const m0[] = { "RPUSH", "m", "z", NULL };
	static const char *const *const s_list[] = { m0, NULL };
	static const char *const hset[] = { "HSET", "h", "f1", "1", "f2",
	    "2", "f3", "3", NULL };
	static const char *const h0[] = { "HSET", "h", "f0", "0", "f1", "x",
	    NULL };
	static const char *const *const s_hset[] = { h0, NULL };
	static const char *const hdel[] = { "HDEL", "h", "f0", "f1", "f2",
	    NULL };
	static const char *const h1[] = { "HSET", "h", "f0", "0", "f1", "1",
	    "f2", "2", "f3", "3", NULL };
	static const char *const *const s_hdel[] = { h1, NULL };
	static const char *const del[] = { "DEL", "a", "b", "c", "l", "h",
	    NULL };
	static const char *const sa[] = { "SET", "a", "1", NULL };
	static const char *const sb[] = { "SET", "b", "2", NULL };
	static const char *const sc[] = { "SET", "c", "3", NULL };
	static const char *const sl[] = { "RPUSH", "l", "x", "y", NULL };
	static const char *const sh[] = { "HSET", "h", "k", "v", NULL };
	static const char *const *const s_del[] = { sa, sb, sc, sl, sh, NULL };
	struct { const char *const *const *setup; const char *const *cmd;
	    const char *const *const *probes; } cases[] = {
		{ none, rpush, plist },
		{ s_list, lpush, plist },
		{ s_hset, hset, phash },
		{ s_hdel, hdel, phash },
		{ s_del, del, pdel },
	};
	size_t c;
	(void)p;

	for (c = 0; c < sizeof cases / sizeof cases[0]; c++)
		midcmd_case(t, cases[c].setup, cases[c].cmd, cases[c].probes);
	return MUNIT_OK;
}

static char *kill_only[] = { "kill", NULL };
static MunitParameterEnum kill_params[] = {
	{ "stop", kill_only },
	{ NULL, NULL }
};

static MunitTest tests[] = {
	{ "/counters", test_counters, rt_setup, rt_teardown, MUNIT_TEST_OPTION_NONE, params },
	{ "/ttl",      test_ttl,      rt_setup, rt_teardown, MUNIT_TEST_OPTION_NONE, params },
	{ "/ttl_range", test_ttl_range, rt_setup, rt_teardown, MUNIT_TEST_OPTION_NONE, params },
	{ "/flush",    test_flush,    rt_setup, rt_teardown, MUNIT_TEST_OPTION_NONE, params },
	{ "/list",     test_list,     rt_setup, rt_teardown, MUNIT_TEST_OPTION_NONE, params },
	{ "/hash",     test_hash,     rt_setup, rt_teardown, MUNIT_TEST_OPTION_NONE, params },
	{ "/del",      test_del,      rt_setup, rt_teardown, MUNIT_TEST_OPTION_NONE, params },
	{ "/midcmd",   test_midcmd,   rt_setup, rt_teardown, MUNIT_TEST_OPTION_NONE, kill_params },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m99/rexis_restart", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
