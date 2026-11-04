/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/main.c -- l2book: the venue, its self-test and its benchmark.
 *
 *	l2book [options]            run the venue (Ctrl-C to stop)
 *	l2book --self-test          start it on loopback, drive orders over
 *	                            TCP, check the reports and the market
 *	                            data, exit 0 on success
 *	l2book --bench N            N orders round-trip over loopback TCP
 *	                            per client; prints the RTT percentiles
 *	                            and the per-stage breakdown
 */
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "venue.h"

void venue_set_ready_fd(int fd);

static void
usage(FILE *f)
{
	fprintf(f,
	    "Usage: l2book [options]\n"
	    "\n"
	    "A level-2 limit order book venue on libxtc: one stackless Isolate\n"
	    "per symbol owns its book; supervised fibers run the sessions, the\n"
	    "market data and the web console.\n"
	    "\n"
	    "  -s, --shards=N     book shards (cores), 1..8 (default 2)\n"
	    "  -y, --symbols=L    comma list (default AAPL,MSFT,NVDA,AMZN,GOOG,META,TSLA,SPY)\n"
	    "  -p, --oe-port=P    order entry TCP port (default 9000; 0 = off)\n"
	    "  -m, --md-port=P    market data TCP port (default 9001; 0 = off)\n"
	    "  -w, --web-port=P   web console HTTP port (default 8080; 0 = off)\n"
	    "  -r, --web-root=D   static files for the console (default ./web)\n"
	    "  -f, --flow         run the built-in market maker + noise flow\n"
	    "      --rate=R       flow commands/s per symbol (default 200)\n"
	    "      --ref=TICKS    reference price in cents (default 10000)\n"
	    "      --self-test    run the end-to-end self-test and exit\n"
	    "      --bench=N      loopback round-trip benchmark, N orders/client\n"
	    "      --help         show this help and exit\n");
}

/* ---- the self-test and bench client (plain threads, blocking TCP) -- */

static int g_oe_port;
static int g_ready_rd = -1;

/* Block until the venue's boot fiber says the listeners are up. */
static void
wait_ready(void)
{
	char x;
	struct pollfd p = { g_ready_rd, POLLIN, 0 };
	if (g_ready_rd >= 0 && poll(&p, 1, 10000) > 0 &&
	    read(g_ready_rd, &x, 1) != 1)
		fprintf(stderr, "l2book: ready pipe\n");
}

static int
dial(int port)
{
	struct sockaddr_in a;
	int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	return fd;
}

static int
xwrite(int fd, const void *b, size_t n)
{
	size_t off = 0;
	while (off < n) {
		ssize_t r = write(fd, (const char *)b + off, n - off);
		if (r <= 0)
			return -1;
		off += (size_t)r;
	}
	return 0;
}

static int
xread(int fd, void *b, size_t n, int timeout_ms)
{
	size_t off = 0;
	while (off < n) {
		struct pollfd p = { fd, POLLIN, 0 };
		ssize_t r;
		if (poll(&p, 1, timeout_ms) <= 0)
			return -1;
		r = read(fd, (char *)b + off, n - off);
		if (r <= 0)
			return -1;
		off += (size_t)r;
	}
	return 0;
}

static unsigned char *
p64(unsigned char *p, uint64_t v)
{
	int i;
	for (i = 0; i < 8; i++)
		*p++ = (unsigned char)(v >> (8 * i));
	return p;
}

static uint64_t
g64(const unsigned char *p)
{
	uint64_t v = 0;
	int i;
	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static int
send_rec(int fd, const unsigned char *rec, uint32_t n)
{
	unsigned char b[300];
	b[0] = (unsigned char)n;
	b[1] = (unsigned char)(n >> 8);
	b[2] = (unsigned char)(n >> 16);
	b[3] = (unsigned char)(n >> 24);
	memcpy(b + 4, rec, n);
	return xwrite(fd, b, 4 + n);
}

/* One reply frame into rec; returns its length or -1. */
static int
recv_rec(int fd, unsigned char *rec, size_t cap, int timeout_ms)
{
	unsigned char h[4];
	uint32_t n;
	if (xread(fd, h, 4, timeout_ms) != 0)
		return -1;
	n = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 |
	    (uint32_t)h[3] << 24;
	if (n > cap || xread(fd, rec, n, timeout_ms) != 0)
		return -1;
	return (int)n;
}

static int
login(int fd, uint32_t account)
{
	unsigned char r[21] = { 'L' };
	r[1] = (unsigned char)account;
	r[2] = (unsigned char)(account >> 8);
	r[3] = (unsigned char)(account >> 16);
	r[4] = (unsigned char)(account >> 24);
	return send_rec(fd, r, sizeof r);
}

static int
order(int fd, uint64_t cid, uint16_t sym, int side, int type, int tif,
    int64_t price, int64_t qty)
{
	unsigned char r[1 + 8 + 2 + 6 + 5 * 8 + 8 + 8], *p = r;
	memset(r, 0, sizeof r);
	*p++ = 'O';
	p = p64(p, cid);
	*p++ = (unsigned char)sym;
	*p++ = (unsigned char)(sym >> 8);
	*p++ = (unsigned char)side;
	*p++ = (unsigned char)type;
	*p++ = (unsigned char)tif;
	*p++ = 0;	/* stp */
	*p++ = 0;	/* flags */
	*p++ = 0;	/* peg */
	p = p64(p, (uint64_t)price);
	p = p64(p, 0);
	p = p64(p, (uint64_t)qty);
	return send_rec(fd, r, sizeof r);
}

static int
cancel(int fd, uint64_t cid, uint16_t sym)
{
	unsigned char r[11] = { 'X' };
	p64(r + 1, cid);
	r[9] = (unsigned char)sym;
	r[10] = (unsigned char)(sym >> 8);
	return send_rec(fd, r, sizeof r);
}

static uint64_t
mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int failures;
#define T(c) do { if (!(c)) { fprintf(stderr, "  self-test FAIL %s:%d: %s\n", \
	__func__, __LINE__, #c); failures++; goto done; } } while (0)

/* Wait for a reply of type `want` for client id cid (skipping others). */
static int
expect(int fd, char want, uint64_t cid, unsigned char *rec)
{
	int i;
	for (i = 0; i < 64; i++) {
		int n = recv_rec(fd, rec, 256, 3000);
		if (n < 0)
			return -1;
		if (rec[0] == want && (cid == 0 || g64(rec + 1) == cid))
			return n;
	}
	return -1;
}

static void *
self_test(void *arg)
{
	int a = -1, b = -1, md = -1;
	unsigned char rec[256];
	int n, i, saw_level = 0, saw_trade = 0;
	(void)arg;

	wait_ready();
	a = dial(g_oe_port);
	b = dial(g_oe_port);
	T(a >= 0 && b >= 0);
	T(login(a, 11) == 0 && login(b, 12) == 0);
	/* Account 11 rests a sell; 12 crosses it. */
	T(order(a, 1, 0, BOOK_SELL, BOOK_LIMIT, BOOK_GTC, 10050, 300) == 0);
	n = expect(a, 'A', 1, rec);
	T(n > 0);
	T(order(b, 7, 0, BOOK_BUY, BOOK_LIMIT, BOOK_IOC, 10050, 100) == 0);
	n = expect(b, 'E', 7, rec);
	T(n > 0 && (int64_t)g64(rec + 12) == 10050 && g64(rec + 20) == 100 &&
	    rec[28] == 'R');
	n = expect(a, 'E', 1, rec);			/* the maker's fill */
	T(n > 0 && g64(rec + 20) == 100 && rec[28] == 'A');
	/* A cannot cancel B's orders; A cancels its own remainder. */
	T(cancel(a, 7, 0) == 0);
	n = expect(a, 'J', 7, rec);
	T(n > 0);
	T(cancel(a, 1, 0) == 0);
	n = expect(a, 'D', 1, rec);
	T(n > 0 && g64(rec + 11) == 200);
	/* Market data: a subscriber sees the snapshot and later changes. */
	md = dial(g_oe_port + 1);
	T(md >= 0);
	(void)usleep(50000);
	T(order(a, 2, 0, BOOK_BUY, BOOK_LIMIT, BOOK_GTC, 9990, 50) == 0);
	T(expect(a, 'A', 2, rec) > 0);
	T(order(b, 8, 0, BOOK_SELL, BOOK_MARKET, BOOK_IOC, 0, 50) == 0);
	T(expect(b, 'E', 8, rec) > 0);
	for (i = 0; i < 200 && !(saw_level && saw_trade); i++) {
		n = recv_rec(md, rec, sizeof rec, 2000);
		if (n < 0)
			break;
		if (rec[0] == 'B' && (int64_t)g64(rec + 4) == 9990)
			saw_level = 1;
		if (rec[0] == 'T' && (int64_t)g64(rec + 4) == 9990)
			saw_trade = 1;
	}
	T(saw_level && saw_trade);
	/* Cancel on disconnect: B rests, disconnects, the level clears. */
	T(order(b, 9, 1, BOOK_BUY, BOOK_LIMIT, BOOK_GTC, 9000, 10) == 0);
	T(expect(b, 'A', 9, rec) > 0);
	close(b);
	b = -1;
	for (i = 0; i < 200; i++) {
		n = recv_rec(md, rec, sizeof rec, 2000);
		T(n > 0);
		if (rec[0] == 'B' && (int64_t)g64(rec + 4) == 9000 &&
		    g64(rec + 12) == 0)
			break;
	}
	T(i < 200);
	fprintf(stderr, "  self-test: order entry, fills both sides, "
	    "ownership, cancel, market data, cancel-on-disconnect: OK\n");
done:
	if (a >= 0)
		close(a);
	if (b >= 0)
		close(b);
	if (md >= 0)
		close(md);
	xtc_tnt_stop();
	return NULL;
}

/* ---- bench: N round trips per client, over loopback TCP ------------ */

static long g_bench_n;
static int g_bench_clients = 4;
static hist_t g_rtt;
static pthread_mutex_t g_rtt_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_bench_done;

struct bench_arg {
	int id;
};

static void *
bench_client(void *arg)
{
	struct bench_arg *ba = arg;
	static hist_t local[16];
	hist_t *h = &local[ba->id];
	int fd = dial(g_oe_port), sym = ba->id % venue_nsyms();
	unsigned char rec[256];
	long i;

	memset(h, 0, sizeof *h);
	if (fd < 0 || login(fd, (uint32_t)(200 + ba->id)) != 0)
		goto out;
	for (i = 0; i < g_bench_n; i++) {
		uint64_t cid = (uint64_t)i + 1, t0;
		int side = (int)(i & 1);
		/* A resting order away from the touch, then its cancel: two
		 * round trips, the cancel-heavy shape. */
		int64_t px = side == BOOK_BUY ? 9800 - (i % 50) : 10200 + (i % 50);
		t0 = mono_ns();
		if (order(fd, cid, (uint16_t)sym, side, BOOK_LIMIT, BOOK_GTC,
		    px, 100) != 0 || expect(fd, 'A', cid, rec) < 0)
			goto out;
		hist_record(h, mono_ns() - t0);
		t0 = mono_ns();
		if (cancel(fd, cid, (uint16_t)sym) != 0 ||
		    expect(fd, 'D', cid, rec) < 0)
			goto out;
		hist_record(h, mono_ns() - t0);
	}
out:
	pthread_mutex_lock(&g_rtt_lock);
	hist_merge(&g_rtt, h);
	pthread_mutex_unlock(&g_rtt_lock);
	if (fd >= 0)
		close(fd);
	if (atomic_fetch_add(&g_bench_done, 1) + 1 == g_bench_clients)
		xtc_tnt_stop();
	return NULL;
}

static void *
bench_main(void *arg)
{
	static struct bench_arg ba[16];
	pthread_t th[16];
	uint64_t t0, t1;
	int i;
	(void)arg;
	wait_ready();
	t0 = mono_ns();
	for (i = 0; i < g_bench_clients; i++) {
		ba[i].id = i;
		(void)pthread_create(&th[i], NULL, bench_client, &ba[i]);
	}
	for (i = 0; i < g_bench_clients; i++)
		(void)pthread_join(th[i], NULL);
	t1 = mono_ns();
	printf("l2book bench: %d clients x %ld order+cancel over loopback TCP, "
	    "%.0f round trips/s\n", g_bench_clients, g_bench_n,
	    (double)g_rtt.n / ((double)(t1 - t0) / 1e9));
	printf("  round trip (client send -> report read), ns: p50 %llu  "
	    "p99 %llu  p999 %llu  max %llu\n",
	    (unsigned long long)hist_q(&g_rtt, 0.50),
	    (unsigned long long)hist_q(&g_rtt, 0.99),
	    (unsigned long long)hist_q(&g_rtt, 0.999),
	    (unsigned long long)g_rtt.max);
	for (i = 0; i < venue_nsyms() && i < g_bench_clients; i++) {
		struct venue_stats *s = venue_sym_stats(i);
		printf("  %-5s queue (session -> book) p50 %llu p99 %llu | "
		    "match p50 %llu p99 %llu p999 %llu\n", venue_sym_name(i),
		    (unsigned long long)hist_q(&s->queue, 0.50),
		    (unsigned long long)hist_q(&s->queue, 0.99),
		    (unsigned long long)hist_q(&s->match, 0.50),
		    (unsigned long long)hist_q(&s->match, 0.99),
		    (unsigned long long)hist_q(&s->match, 0.999));
	}
	return NULL;
}

/* ---- main ---------------------------------------------------------- */

static void
on_signal(int sig)
{
	(void)sig;
	xtc_tnt_stop();
}

int
main(int argc, char **argv)
{
	enum { O_HELP = 1000, O_SELF, O_BENCH, O_RATE, O_REF };
	static const struct option lo[] = {
		{ "shards", required_argument, NULL, 's' },
		{ "symbols", required_argument, NULL, 'y' },
		{ "oe-port", required_argument, NULL, 'p' },
		{ "md-port", required_argument, NULL, 'm' },
		{ "web-port", required_argument, NULL, 'w' },
		{ "web-root", required_argument, NULL, 'r' },
		{ "flow", no_argument, NULL, 'f' },
		{ "rate", required_argument, NULL, O_RATE },
		{ "ref", required_argument, NULL, O_REF },
		{ "self-test", no_argument, NULL, O_SELF },
		{ "bench", required_argument, NULL, O_BENCH },
		{ "help", no_argument, NULL, O_HELP },
		{ NULL, 0, NULL, 0 }
	};
	venue_opts_t o;
	pthread_t driver;
	int c, rc, pfd[2];

	memset(&o, 0, sizeof o);
	o.shards = 2;
	o.oe_port = 9000;
	o.md_port = 9001;
	o.web_port = 8080;
	o.web_root = "web";
	o.symbols = "AAPL,MSFT,NVDA,AMZN,GOOG,META,TSLA,SPY";
	o.flow_rate = 200;
	o.ref_price = 10000;
	while ((c = getopt_long(argc, argv, "s:y:p:m:w:r:f", lo, NULL)) != -1) {
		switch (c) {
		case 's': o.shards = atoi(optarg); break;
		case 'y': o.symbols = optarg; break;
		case 'p': o.oe_port = atoi(optarg); break;
		case 'm': o.md_port = atoi(optarg); break;
		case 'w': o.web_port = atoi(optarg); break;
		case 'r': o.web_root = optarg; break;
		case 'f': o.flow = 1; break;
		case O_RATE: o.flow_rate = atof(optarg); break;
		case O_REF: o.ref_price = atoll(optarg); break;
		case O_SELF: o.self_test = 1; break;
		case O_BENCH: o.bench_ops = atol(optarg); break;
		case O_HELP: usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	if (o.shards < 1 || o.shards > VENUE_MAX_SHARDS) {
		fprintf(stderr, "l2book: --shards must be 1..%d\n",
		    VENUE_MAX_SHARDS);
		return 2;
	}
	if (o.self_test || o.bench_ops > 0) {
		/* Ephemeral-ish ports so a test does not collide with a
		 * running venue; no web, no flow. */
		o.oe_port = 19000 + (int)(getpid() % 2000) * 2;
		o.md_port = o.oe_port + 1;
		o.web_port = 0;
		o.flow = 0;
	}
	g_oe_port = o.oe_port;
	g_bench_n = o.bench_ops;
	(void)signal(SIGINT, on_signal);
	(void)signal(SIGTERM, on_signal);
	(void)signal(SIGPIPE, SIG_IGN);
	if (o.self_test || o.bench_ops > 0) {
		/* The driver waits until the venue says it is up. */
		if (pipe(pfd) != 0)
			return 1;
		g_ready_rd = pfd[0];
		venue_set_ready_fd(pfd[1]);
		if (pthread_create(&driver, NULL, o.self_test ? self_test :
		    bench_main, NULL) != 0)
			return 1;
	}
	rc = venue_main(&o);
	if (o.self_test || o.bench_ops > 0)
		(void)pthread_join(driver, NULL);
	if (o.self_test) {
		if (failures) {
			fprintf(stderr, "l2book self-test: %d failure(s)\n",
			    failures);
			return 1;
		}
		fprintf(stderr, "l2book self-test: passed\n");
	}
	return rc;
}
