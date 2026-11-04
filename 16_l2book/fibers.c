/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/fibers.c -- everything that blocks: sessions, market data,
 * the web console, and the flow generator, as supervised fibers.
 *
 *	The fibers run on ONE standalone loop on its own OS thread, next to
 *	the book shards.  Not an xtc_exec: a pid is executor-relative
 *	(loop_id = exec slot + 1), and xtc_tnt runs the books on an exec of
 *	its own, so a book's xtc_send to a fiber on a second exec resolves
 *	to the same-numbered loop of the BOOK's exec and is misdelivered
 *	(libxtc finding, .agent/l2book-2026-11/FINDING-twoexec-pid.md).  A
 *	standalone loop's pids (loop_id 0) resolve correctly from any exec.
 *	One loop is plenty: a session fiber does a recv, a decode and a ring
 *	push per command; the matching runs on the shards.  Each connection is one
 *	xtc_proc: straight-line code that reads a frame, sends a command to
 *	a book Isolate, and parks on xtc_proc_wait_fd -- which returns on
 *	socket readiness OR on a message in the proc's mailbox, so the same
 *	fiber writes back the execution reports the book sends it.  A
 *	listener per port accepts and spawns connection fibers under a
 *	simple_one_for_one supervisor; a crash in one session takes down
 *	that session only.
 */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "venue.h"
#include "xtc_loop.h"
#include "xtc_net.h"
#include "xtc_orc.h"


_Atomic int venue_stopping;

static xtc_loop_t *g_loop;
static pthread_t g_loop_thread;
static const venue_opts_t *g_opts;
static _Atomic uint32_t g_next_account = 100;
static int g_ready_fd = -1;

int64_t venue_sym_best(int i, int side);
int64_t venue_sym_last(int i);

/* ---- small socket helpers (non-blocking fds, parked in fibers) ----- */

static int
write_full(int fd, const void *buf, size_t n)
{
	size_t off = 0;
	while (off < n) {
		ssize_t r = send(fd, (const char *)buf + off, n - off,
		    MSG_NOSIGNAL);
		if (r > 0) {
			off += (size_t)r;
			continue;
		}
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			uint32_t rev = 0;
			if (atomic_load(&venue_stopping))
				return -1;
			(void)xtc_proc_wait_fd(fd, XTC_IO_WRITABLE,
			    200LL * 1000 * 1000, &rev);
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

/* One length-prefixed frame (u32 LE length, then the record). */
static int
send_frame(int fd, const void *rec, uint32_t len)
{
	unsigned char hdr[4] = { (unsigned char)len, (unsigned char)(len >> 8),
	    (unsigned char)(len >> 16), (unsigned char)(len >> 24) };
	struct iovec v[2];
	ssize_t r;
	v[0].iov_base = hdr;
	v[0].iov_len = 4;
	v[1].iov_base = (void *)rec;
	v[1].iov_len = len;
	r = writev(fd, v, 2);
	if (r == (ssize_t)(4 + len))
		return 0;
	if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
		return -1;
	/* Short or would-block: finish the rest the slow way. */
	if (r < 0)
		r = 0;
	if ((size_t)r < 4) {
		if (write_full(fd, hdr + r, 4 - (size_t)r) != 0)
			return -1;
		r = 0;
	} else
		r -= 4;
	return write_full(fd, (const char *)rec + r, len - (size_t)r);
}

/* Little-endian field access. */
static uint64_t
le64(const unsigned char *p)
{
	uint64_t v = 0;
	int i;
	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static uint32_t
le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24;
}

static uint16_t
le16(const unsigned char *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static unsigned char *
put64(unsigned char *p, uint64_t v)
{
	int i;
	for (i = 0; i < 8; i++)
		*p++ = (unsigned char)(v >> (8 * i));
	return p;
}

static unsigned char *
put16(unsigned char *p, uint16_t v)
{
	*p++ = (unsigned char)v;
	*p++ = (unsigned char)(v >> 8);
	return p;
}

/* ---- the order-entry session fiber ---------------------------------- */

#define REC_MAX 256

/* Encode one execution report (PROTOCOL.md, venue to client). */
static uint32_t
encode_report(const struct venue_report *r, unsigned char *b)
{
	unsigned char *p = b;
	switch (r->kind) {
	case BOOK_EV_ACCEPTED:
		*p++ = 'A';
		p = put64(p, r->client_id);
		p = put16(p, r->symbol);
		*p++ = r->side;
		p = put64(p, (uint64_t)r->price);
		p = put64(p, (uint64_t)r->qty);
		break;
	case BOOK_EV_TRADE:
		*p++ = 'E';
		p = put64(p, r->client_id);
		p = put16(p, r->symbol);
		*p++ = r->side;
		p = put64(p, (uint64_t)r->price);
		p = put64(p, (uint64_t)r->qty);
		*p++ = r->liquidity;
		break;
	case BOOK_EV_CANCELED:
		*p++ = 'D';
		p = put64(p, r->client_id);
		p = put16(p, r->symbol);
		p = put64(p, (uint64_t)r->qty);
		*p++ = r->reason;
		break;
	case BOOK_EV_REJECTED:
		*p++ = 'J';
		p = put64(p, r->client_id);
		p = put16(p, r->symbol);
		*p++ = r->reason;
		break;
	case BOOK_EV_REDUCED:
		*p++ = 'M';
		p = put64(p, r->client_id);
		p = put16(p, r->symbol);
		p = put64(p, (uint64_t)r->qty);
		break;
	case BOOK_EV_REPLACED:
		*p++ = 'P';
		p = put64(p, r->client_id);
		p = put16(p, r->symbol);
		p = put64(p, r->other);
		break;
	default:
		return 0;	/* TRIGGERED etc.: not on the wire */
	}
	p = put64(p, r->seq);
	return (uint32_t)(p - b);
}

/* Decode one client record into a command.  Returns the tag, or 0. */
static uint16_t
decode_cmd(const unsigned char *b, uint32_t n, uint32_t account,
    struct venue_cmd *c, int *sym)
{
	memset(c, 0, sizeof *c);
	c->account = account;
	c->reply = xtc_self();
	if (n < 1)
		return 0;
	switch (b[0]) {
	case 'O':
		if (n < 1 + 8 + 2 + 6 + 5 * 8 + 8 + 8)
			return 0;
		c->client_id = le64(b + 1);
		*sym = le16(b + 9);
		c->side = b[11];
		c->type = b[12];
		c->tif = b[13];
		c->stp = b[14];
		c->flags = b[15];
		c->peg = b[16];
		c->price = (int64_t)le64(b + 17);
		c->stop = (int64_t)le64(b + 25);
		c->qty = (int64_t)le64(b + 33);
		c->display = (int64_t)le64(b + 41);
		/* peg_offset (b + 49) rides in stop for pegged orders */
		if (c->type == BOOK_PEGGED)
			c->stop = (int64_t)le64(b + 49);
		c->aux = le64(b + 57);
		return VT_ORDER;
	case 'X':
		if (n < 11)
			return 0;
		c->client_id = le64(b + 1);
		*sym = le16(b + 9);
		return VT_CANCEL;
	case 'R':
		if (n < 19)
			return 0;
		c->client_id = le64(b + 1);
		*sym = le16(b + 9);
		c->qty = (int64_t)le64(b + 11);
		return VT_REDUCE;
	case 'U':
		if (n < 35)
			return 0;
		c->client_id = le64(b + 1);
		*sym = le16(b + 9);
		c->aux = le64(b + 11);
		c->price = (int64_t)le64(b + 19);
		c->qty = (int64_t)le64(b + 27);
		return VT_REPLACE;
	case 'C':
		*sym = -1;
		return VT_CANCEL_ALL;
	}
	return 0;
}

/* Drain the mailbox: every report the books sent, written back. */
static int
session_drain(int fd)
{
	void *m;
	size_t sz;
	unsigned char rec[REC_MAX];
	while (xtc_recv(&m, &sz, 0) == XTC_OK) {
		int rc = 0;
		if (sz == sizeof(struct venue_report)) {
			uint32_t n = encode_report(m, rec);
			if (n > 0)
				rc = send_frame(fd, rec, n);
		}
		xtc_free(m);
		if (rc != 0)
			return -1;
	}
	return 0;
}

struct conn_args {
	int fd;
};

/* Handle one complete client record. 0 = ok, -1 = drop the session. */
static int
session_record(int fd, const unsigned char *rec, uint32_t len,
    uint32_t *account)
{
	struct venue_cmd c;
	unsigned char out[REC_MAX];
	int sym = -1;
	uint16_t tag;

	if (rec[0] == 'L' && len >= 5) {
		*account = le32(rec + 1);
		if (*account == 0)
			*account = atomic_fetch_add(&g_next_account, 1);
		return 0;
	}
	if (*account == 0)
		return -1;		/* login first */
	tag = decode_cmd(rec, len, *account, &c, &sym);
	if (tag == 0)
		return -1;
	c.t_ingress = venue_now_ns();
	if (tag == VT_CANCEL_ALL) {
		int i;
		for (i = 0; i < venue_nsyms(); i++)
			(void)venue_to_book(i, tag, &c);
		return 0;
	}
	if (venue_to_book(sym, tag, &c) != XTC_TNT_SEND_OK) {
		/* The book's ring is full (or the symbol is unknown): reject
		 * here, visibly, rather than queue without bound. */
		struct venue_report rj;
		memset(&rj, 0, sizeof rj);
		rj.kind = BOOK_EV_REJECTED;
		rj.client_id = c.client_id;
		rj.symbol = (uint16_t)(sym < 0 ? 0 : sym);
		rj.reason = BOOK_R_FULL;
		return send_frame(fd, out, encode_report(&rj, out));
	}
	return 0;
}

static void
session_fiber(void *arg)
{
	struct conn_args *ca = arg;
	int fd = ca->fd;
	uint32_t account = 0;
	/* Read buffer: frames are parsed out of it as they complete. */
	unsigned char buf[8192];
	size_t have = 0;

	xtc_free(ca);
	while (!atomic_load(&venue_stopping)) {
		uint32_t rev = 0;
		ssize_t r;
		size_t off;

		if (session_drain(fd) != 0)
			break;
		r = recv(fd, buf + have, sizeof buf - have, 0);
		if (r == 0)
			break;
		if (r < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK &&
			    errno != EINTR)
				break;
			/* Park on the socket AND the mailbox: a report from a
			 * book wakes us as readily as a client byte. */
			(void)xtc_proc_wait_fd(fd, XTC_IO_READABLE,
			    500LL * 1000 * 1000, &rev);
			continue;
		}
		have += (size_t)r;
		for (off = 0; have - off >= 4; ) {
			uint32_t len = le32(buf + off);
			if (len == 0 || len > REC_MAX)
				goto out;
			if (have - off < 4 + len)
				break;
			if (session_record(fd, buf + off + 4, len, &account) != 0)
				goto out;
			off += 4 + len;
		}
		if (off > 0) {
			memmove(buf, buf + off, have - off);
			have -= off;
		}
	}
out:
	if (account != 0) {
		/* Cancel on disconnect. */
		struct venue_cmd c;
		int i;
		memset(&c, 0, sizeof c);
		c.account = account;
		c.reply = xtc_self();
		for (i = 0; i < venue_nsyms(); i++)
			(void)venue_to_book(i, VT_CANCEL_ALL, &c);
	}
	xtc_net_close(fd);
}

/* ---- market-data subscriber fiber (binary, port md_port) ------------ */

static uint32_t
encode_md(const struct venue_md *m, unsigned char *b)
{
	unsigned char *p = b;
	*p++ = m->kind;
	p = put16(p, m->symbol);
	switch (m->kind) {
	case 'B':
		*p++ = m->side;
		p = put64(p, (uint64_t)m->price);
		p = put64(p, (uint64_t)m->qty);
		break;
	case 'T':
		*p++ = m->side;
		p = put64(p, (uint64_t)m->price);
		p = put64(p, (uint64_t)m->qty);
		break;
	case 'H':
		*p++ = (unsigned char)m->qty;
		break;
	case 'S':
		p = put64(p, (uint64_t)m->price);	/* last trade */
		break;
	}
	p = put64(p, m->seq);
	return (uint32_t)(p - b);
}

static void
subscribe_all(int on)
{
	struct venue_cmd c;
	int i;
	memset(&c, 0, sizeof c);
	c.reply = xtc_self();
	for (i = 0; i < venue_nsyms(); i++)
		(void)venue_to_book(i, on ? VT_SUBSCRIBE : VT_UNSUBSCRIBE, &c);
}

static void
md_fiber(void *arg)
{
	struct conn_args *ca = arg;
	int fd = ca->fd;
	unsigned char rec[64];

	xtc_free(ca);
	subscribe_all(1);
	while (!atomic_load(&venue_stopping)) {
		void *m;
		size_t sz;
		int rc = xtc_recv(&m, &sz, 500LL * 1000 * 1000);
		if (rc == XTC_E_AGAIN)
			continue;
		if (rc != XTC_OK)
			break;
		rc = sz == sizeof(struct venue_md) ?
		    send_frame(fd, rec, encode_md(m, rec)) : 0;
		xtc_free(m);
		if (rc != 0)
			break;
	}
	subscribe_all(0);
	xtc_net_close(fd);
}

/* ---- the web console: HTTP + SSE ----------------------------------- */

static const char *
mime(const char *path)
{
	size_t n = strlen(path);
	if (n > 5 && strcmp(path + n - 5, ".wasm") == 0)
		return "application/wasm";
	if (n > 3 && strcmp(path + n - 3, ".js") == 0)
		return "text/javascript";
	if (n > 4 && strcmp(path + n - 4, ".css") == 0)
		return "text/css";
	return "text/html; charset=utf-8";
}

static void
http_reply(int fd, int code, const char *type, const void *body, size_t n)
{
	char hdr[256];
	int h = snprintf(hdr, sizeof hdr, "HTTP/1.1 %d %s\r\nContent-Type: %s"
	    "\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
	    "Connection: close\r\n\r\n", code, code == 200 ? "OK" : "Error",
	    type, n);
	if (write_full(fd, hdr, (size_t)h) == 0 && n > 0)
		(void)write_full(fd, body, n);
}

static void
serve_file(int fd, const char *rel)
{
	char path[512];
	FILE *f;
	long n;
	char *buf;
	if (strstr(rel, "..") != NULL) {
		http_reply(fd, 403, "text/plain", "no\n", 3);
		return;
	}
	snprintf(path, sizeof path, "%s/%s", g_opts->web_root ?
	    g_opts->web_root : "web", *rel ? rel : "index.html");
	f = fopen(path, "rb");	/* XTC_BLOCKING_OK: small static files */
	if (f == NULL) {
		http_reply(fd, 404, "text/plain", "not found\n", 10);
		return;
	}
	(void)fseek(f, 0, SEEK_END);
	n = ftell(f);
	(void)fseek(f, 0, SEEK_SET);
	buf = xtc_malloc((size_t)(n > 0 ? n : 1));
	if (buf != NULL && fread(buf, 1, (size_t)n, f) == (size_t)n)
		http_reply(fd, 200, mime(path), buf, (size_t)n);
	else
		http_reply(fd, 500, "text/plain", "read\n", 5);
	xtc_free(buf);
	(void)fclose(f);
}

static size_t
stats_json(char *b, size_t cap)
{
	size_t n = 0;
	int i;
	n += (size_t)snprintf(b + n, cap - n, "{\"shards\":%d,\"symbols\":[",
	    g_opts->shards);
	for (i = 0; i < venue_nsyms() && n < cap - 512; i++) {
		struct venue_stats *s = venue_sym_stats(i);
		n += (size_t)snprintf(b + n, cap - n,
		    "%s{\"sym\":\"%s\",\"bid\":%lld,\"ask\":%lld,\"last\":%lld,"
		    "\"commands\":%llu,\"trades\":%llu,\"volume\":%llu,"
		    "\"rejects\":%llu,\"drops\":%llu,"
		    "\"match_ns\":{\"p50\":%llu,\"p99\":%llu,\"p999\":%llu},"
		    "\"queue_ns\":{\"p50\":%llu,\"p99\":%llu,\"p999\":%llu}}",
		    i ? "," : "", venue_sym_name(i),
		    (long long)venue_sym_best(i, BOOK_BUY),
		    (long long)venue_sym_best(i, BOOK_SELL),
		    (long long)venue_sym_last(i),
		    (unsigned long long)atomic_load(&s->commands),
		    (unsigned long long)atomic_load(&s->trades),
		    (unsigned long long)atomic_load(&s->volume),
		    (unsigned long long)atomic_load(&s->rejects),
		    (unsigned long long)atomic_load(&s->drops),
		    (unsigned long long)hist_q(&s->match, 0.50),
		    (unsigned long long)hist_q(&s->match, 0.99),
		    (unsigned long long)hist_q(&s->match, 0.999),
		    (unsigned long long)hist_q(&s->queue, 0.50),
		    (unsigned long long)hist_q(&s->queue, 0.99),
		    (unsigned long long)hist_q(&s->queue, 0.999));
	}
	n += (size_t)snprintf(b + n, cap - n, "]}\n");
	return n;
}

static int
query_get(const char *q, const char *key, char *out, size_t cap)
{
	size_t k = strlen(key);
	while (q && *q) {
		if (strncmp(q, key, k) == 0 && q[k] == '=') {
			size_t n = strcspn(q + k + 1, "& \r\n");
			if (n >= cap)
				n = cap - 1;
			memcpy(out, q + k + 1, n);
			out[n] = '\0';
			return 1;
		}
		q = strchr(q, '&');
		if (q)
			q++;
	}
	return 0;
}

/* The console's SSE stream: one fiber per browser, subscribed to the
 * symbol's book, writing each update as a JSON event. */
static void
sse_stream(int fd, int sym)
{
	static const char hdr[] = "HTTP/1.1 200 OK\r\nContent-Type: "
	    "text/event-stream\r\nCache-Control: no-store\r\n"
	    "Connection: keep-alive\r\nAccess-Control-Allow-Origin: *\r\n\r\n"
	    "retry: 1000\n\n";
	struct venue_cmd c;
	char line[256];
	int64_t last_stats = 0;

	if (write_full(fd, hdr, sizeof hdr - 1) != 0)
		return;
	memset(&c, 0, sizeof c);
	c.reply = xtc_self();
	(void)venue_to_book(sym, VT_SUBSCRIBE, &c);
	while (!atomic_load(&venue_stopping)) {
		void *m;
		size_t sz;
		int n = 0, rc = xtc_recv(&m, &sz, 250LL * 1000 * 1000);
		int64_t now = xtc_clock_mono();
		if (rc == XTC_OK) {
			const struct venue_md *u = m;
			if (sz == sizeof *u) {
				if (u->kind == 'B')
					n = snprintf(line, sizeof line,
					    "event: level\ndata: {\"s\":%u,\"p\":%lld,"
					    "\"q\":%lld,\"seq\":%llu}\n\n", u->side,
					    (long long)u->price, (long long)u->qty,
					    (unsigned long long)u->seq);
				else if (u->kind == 'T')
					n = snprintf(line, sizeof line,
					    "event: trade\ndata: {\"s\":%u,\"p\":%lld,"
					    "\"q\":%lld,\"seq\":%llu}\n\n", u->side,
					    (long long)u->price, (long long)u->qty,
					    (unsigned long long)u->seq);
				else if (u->kind == 'S')
					n = snprintf(line, sizeof line,
					    "event: book\ndata: {\"sym\":\"%s\","
					    "\"last\":%lld,\"seq\":%llu}\n\n",
					    venue_sym_name(sym), (long long)u->price,
					    (unsigned long long)u->seq);
				else if (u->kind == 'H')
					n = snprintf(line, sizeof line,
					    "event: phase\ndata: {\"phase\":%lld}\n\n",
					    (long long)u->qty);
			}
			xtc_free(m);
		} else if (rc != XTC_E_AGAIN)
			break;
		if (n > 0 && write_full(fd, line, (size_t)n) != 0)
			break;
		if (now - last_stats > 1000LL * 1000 * 1000) {
			static char js[16384];
			size_t k = stats_json(js, sizeof js);
			last_stats = now;
			if (write_full(fd, "event: stats\ndata: ", 19) != 0 ||
			    write_full(fd, js, k - 1) != 0 ||
			    write_full(fd, "\n\n", 2) != 0)
				break;
		}
	}
	(void)venue_to_book(sym, VT_UNSUBSCRIBE, &c);
}

static void
web_order(int fd, const char *q)
{
	char v[32];
	struct venue_cmd c;
	int sym;
	static _Atomic uint64_t next_cid = 1;
	char body[128];
	int n;

	memset(&c, 0, sizeof c);
	c.account = 1;		/* the console's "you" account */
	c.reply = XTC_PID_NONE;	/* reports go to the md stream only */
	sym = query_get(q, "sym", v, sizeof v) ? venue_sym_index(v) : 0;
	c.side = (uint8_t)(query_get(q, "side", v, sizeof v) && v[0] == 's');
	c.type = (uint8_t)(query_get(q, "type", v, sizeof v) && v[0] == 'm' ?
	    BOOK_MARKET : BOOK_LIMIT);
	c.tif = (uint8_t)(query_get(q, "tif", v, sizeof v) && v[0] == 'i' ?
	    BOOK_IOC : BOOK_GTC);
	c.price = query_get(q, "price", v, sizeof v) ? atoll(v) : 0;
	c.qty = query_get(q, "qty", v, sizeof v) ? atoll(v) : 100;
	c.client_id = atomic_fetch_add(&next_cid, 1);
	c.t_ingress = venue_now_ns();
	n = snprintf(body, sizeof body, "{\"ok\":%d,\"id\":%llu}\n",
	    sym >= 0 && venue_to_book(sym, VT_ORDER, &c) == XTC_TNT_SEND_OK,
	    (unsigned long long)c.client_id);
	http_reply(fd, 200, "application/json", body, (size_t)n);
}

static void
web_fiber(void *arg)
{
	struct conn_args *ca = arg;
	int fd = ca->fd;
	char req[2048];
	size_t have = 0;
	char *path, *q;

	xtc_free(ca);
	/* Read the request line and headers. */
	while (have < sizeof req - 1) {
		ssize_t r = recv(fd, req + have, sizeof req - 1 - have, 0);
		if (r > 0) {
			have += (size_t)r;
			req[have] = '\0';
			if (strstr(req, "\r\n\r\n") != NULL)
				break;
			continue;
		}
		if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK &&
		    errno != EINTR))
			goto out;
		{
			uint32_t rev = 0;
			if (xtc_proc_wait_fd(fd, XTC_IO_READABLE,
			    5LL * 1000 * 1000 * 1000, &rev) != XTC_OK ||
			    atomic_load(&venue_stopping))
				goto out;
		}
	}
	req[have] = '\0';
	if (strncmp(req, "GET ", 4) != 0 && strncmp(req, "POST ", 5) != 0)
		goto out;
	path = strchr(req, ' ') + 1;
	path[strcspn(path, " ")] = '\0';
	q = strchr(path, '?');
	if (q)
		*q++ = '\0';
	if (strcmp(path, "/healthz") == 0)
		http_reply(fd, 200, "text/plain", "ok\n", 3);
	else if (strcmp(path, "/readyz") == 0) {
		int i, ready = venue_nsyms() > 0;
		for (i = 0; i < venue_nsyms(); i++)
			if (venue_sym_book(i) == XTC_TNT_HANDLE_NONE)
				ready = 0;
		http_reply(fd, ready ? 200 : 503, "text/plain",
		    ready ? "ready\n" : "booting\n", ready ? 6 : 8);
	} else if (strcmp(path, "/stats") == 0) {
		static char js[16384];
		http_reply(fd, 200, "application/json", js,
		    stats_json(js, sizeof js));
	} else if (strcmp(path, "/symbols") == 0) {
		char js[2048];
		size_t n = 0;
		int i;
		n += (size_t)snprintf(js, sizeof js, "[");
		for (i = 0; i < venue_nsyms(); i++)
			n += (size_t)snprintf(js + n, sizeof js - n, "%s\"%s\"",
			    i ? "," : "", venue_sym_name(i));
		n += (size_t)snprintf(js + n, sizeof js - n, "]\n");
		http_reply(fd, 200, "application/json", js, n);
	} else if (strcmp(path, "/md") == 0) {
		char v[16];
		int sym = query_get(q, "sym", v, sizeof v) ? venue_sym_index(v) : 0;
		if (sym < 0)
			sym = 0;
		sse_stream(fd, sym);
	} else if (strcmp(path, "/order") == 0)
		web_order(fd, q);
	else
		serve_file(fd, path + 1);
out:
	xtc_net_close(fd);
}

/* ---- listeners ------------------------------------------------------ */

struct listener {
	int		fd;
	xtc_proc_fn	fiber;
	const char	*name;
};

static struct listener g_listeners[3];
static int g_nlisteners;

static void
listener_fiber(void *arg)
{
	struct listener *l = arg;
	while (!atomic_load(&venue_stopping)) {
		int cfd;
		int rc = xtc_net_accept(l->fd, &cfd);
		if (rc == XTC_OK) {
			struct conn_args *ca = xtc_malloc(sizeof *ca);
			xtc_proc_opts_t po = XTC_PROC_OPTS_INIT;
			xtc_tcp_opts_t t = XTC_TCP_OPTS_DEFAULT;
			xtc_pid_t pid;
			t.nodelay = 1;
			(void)xtc_net_apply_tcp_opts(cfd, &t);
			if (ca == NULL) {
				xtc_net_close(cfd);
				continue;
			}
			ca->fd = cfd;
			po.name = l->name;
			if (xtc_proc_spawn(g_loop, l->fiber, ca, &po, &pid) !=
			    XTC_OK) {
				xtc_free(ca);
				xtc_net_close(cfd);
			}
			continue;
		}
		{
			uint32_t rev = 0;
			(void)xtc_proc_wait_fd(l->fd, XTC_IO_READABLE,
			    200LL * 1000 * 1000, &rev);
		}
	}
	xtc_net_close(l->fd);
}

/* ---- the flow generator: a market maker and noise traders ----------- */

/* Each symbol gets one market maker (an inventory-skewed ladder around
 * a wandering mid, re-quoted every 200 ms) and a noise trader (random
 * resting and marketable orders).  They are ordinary sessions: they
 * reach the books exactly as a TCP client would, through
 * venue_to_book, and read their reports from their mailbox. */
struct flow_args {
	int sym;
};

static uint64_t
xorshift(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static void
flow_fiber(void *arg)
{
	struct flow_args fa = *(struct flow_args *)arg;
	uint64_t rng = 0x9e3779b97f4a7c15ull * (uint64_t)(fa.sym + 1);
	uint32_t mm = atomic_fetch_add(&g_next_account, 2), noise = mm + 1;
	int64_t mid = g_opts->ref_price > 0 ? g_opts->ref_price : 10000;
	int64_t inventory = 0;
	uint64_t cid = 1, quote_ids[10] = { 0 };
	int64_t period = g_opts->flow_rate > 0 ?
	    (int64_t)(1e9 / g_opts->flow_rate) : 5000000;
	int64_t next_quote = 0;

	xtc_free(arg);
	while (!atomic_load(&venue_stopping)) {
		struct venue_cmd c;
		int64_t now = xtc_clock_mono();
		void *m;
		size_t sz;
		int i;

		/* Read our reports: track the maker's inventory. */
		while (xtc_recv(&m, &sz, 0) == XTC_OK) {
			const struct venue_report *r = m;
			if (sz == sizeof *r && r->kind == BOOK_EV_TRADE &&
			    r->liquidity == 'A')
				inventory += r->side == BOOK_BUY ? -r->qty : r->qty;
			xtc_free(m);
		}
		memset(&c, 0, sizeof c);
		c.reply = xtc_self();
		c.symbol = (uint16_t)fa.sym;
		if (now >= next_quote) {
			/* Re-quote: cancel the ladder, walk the mid, lean
			 * against inventory, enter 5 levels each side. */
			int64_t skew = inventory / 200;
			next_quote = now + 200LL * 1000 * 1000;
			c.account = mm;
			for (i = 0; i < 10; i++)
				if (quote_ids[i]) {
					c.client_id = quote_ids[i];
					c.t_ingress = venue_now_ns();
					(void)venue_to_book(fa.sym, VT_CANCEL, &c);
				}
			mid += (int64_t)(xorshift(&rng) % 5) - 2;
			for (i = 0; i < 10; i++) {
				int side = i < 5 ? BOOK_BUY : BOOK_SELL;
				int lvl = i % 5;
				c.client_id = cid++;
				c.side = (uint8_t)side;
				c.type = BOOK_LIMIT;
				c.tif = BOOK_GTC;
				c.qty = 100 * (1 + lvl);
				c.price = side == BOOK_BUY ? mid - 2 - lvl - skew :
				    mid + 2 + lvl - skew;
				c.t_ingress = venue_now_ns();
				quote_ids[i] = venue_to_book(fa.sym, VT_ORDER, &c) ==
				    XTC_TNT_SEND_OK ? c.client_id : 0;
			}
		}
		/* Noise: a resting order near the touch, or a marketable
		 * one (25 %), or a cancel of an old resting order. */
		c.account = noise;
		c.client_id = cid++;
		c.side = (uint8_t)(xorshift(&rng) & 1);
		c.type = BOOK_LIMIT;
		c.qty = 100 * (1 + (int64_t)(xorshift(&rng) % 3));
		if (xorshift(&rng) % 4 == 0) {
			c.tif = BOOK_IOC;
			c.price = c.side == BOOK_BUY ? mid + 10 : mid - 10;
		} else {
			c.tif = BOOK_DAY;
			c.price = c.side == BOOK_BUY ?
			    mid - 1 - (int64_t)(xorshift(&rng) % 8) :
			    mid + 1 + (int64_t)(xorshift(&rng) % 8);
		}
		c.t_ingress = venue_now_ns();
		(void)venue_to_book(fa.sym, VT_ORDER, &c);
		if (cid > 50 && xorshift(&rng) % 2 == 0) {
			c.client_id = cid - 1 - xorshift(&rng) % 40;
			c.t_ingress = venue_now_ns();
			(void)venue_to_book(fa.sym, VT_CANCEL, &c);
		}
		(void)xtc_proc_sleep(period);
	}
}

/* ---- startup -------------------------------------------------------- */

static int
listen_on(int port, xtc_proc_fn fiber, const char *name)
{
	xtc_tcp_opts_t t = XTC_TCP_OPTS_DEFAULT;
	int fd;
	if (port <= 0)
		return 0;
	t.reuseaddr = 1;
	t.nodelay = 1;
	if (xtc_net_listen(XTC_NET_INET, "0.0.0.0", port, &t, &fd) != XTC_OK) {
		fprintf(stderr, "l2book: cannot listen on %d\n", port);
		return -1;
	}
	g_listeners[g_nlisteners].fd = fd;
	g_listeners[g_nlisteners].fiber = fiber;
	g_listeners[g_nlisteners].name = name;
	g_nlisteners++;
	return 0;
}

/* The boot fiber on the fiber exec: wait for the books, then start the
 * listeners and the flow. */
static void
boot_fiber(void *arg)
{
	int i, up;
	xtc_pid_t pid;
	(void)arg;
	do {
		(void)xtc_proc_sleep(5LL * 1000 * 1000);
		for (up = 1, i = 0; i < venue_nsyms(); i++)
			if (venue_sym_book(i) == XTC_TNT_HANDLE_NONE)
				up = 0;
	} while (!up && !atomic_load(&venue_stopping));
	for (i = 0; i < g_nlisteners; i++) {
		xtc_proc_opts_t po = XTC_PROC_OPTS_INIT;
		po.name = "listener";
		(void)xtc_proc_spawn(g_loop, listener_fiber,
		    &g_listeners[i], &po, &pid);
	}
	if (g_opts->flow)
		for (i = 0; i < venue_nsyms(); i++) {
			struct flow_args *fa = xtc_malloc(sizeof *fa);
			xtc_proc_opts_t po = XTC_PROC_OPTS_INIT;
			if (fa == NULL)
				continue;
			fa->sym = i;
			po.name = "flow";
			(void)xtc_proc_spawn(g_loop, flow_fiber, fa, &po, &pid);
		}
	if (g_ready_fd >= 0) {
		char b = 'r';
		if (write(g_ready_fd, &b, 1) != 1)	/* XTC_BLOCKING_OK: 1 byte */
			fprintf(stderr, "l2book: ready pipe\n");
	}
	fprintf(stderr, "l2book: %d symbols on %d shards; order entry :%d, "
	    "market data :%d, console http://0.0.0.0:%d/\n", venue_nsyms(),
	    g_opts->shards, g_opts->oe_port, g_opts->md_port, g_opts->web_port);
}

/* Stops the standalone loop once the venue is stopping. */
static void
stopper_fiber(void *arg)
{
	(void)arg;
	while (!atomic_load(&venue_stopping))
		(void)xtc_proc_sleep(50LL * 1000 * 1000);
	(void)xtc_loop_stop(g_loop);
}

static void *
loop_thread(void *arg)
{
	(void)arg;
	(void)xtc_loop_run(g_loop);
	return NULL;
}

void venue_set_ready_fd(int fd);
void
venue_set_ready_fd(int fd)
{
	g_ready_fd = fd;
}

int
venue_fibers_start(const venue_opts_t *opts)
{
	xtc_pid_t pid;
	xtc_proc_opts_t po = XTC_PROC_OPTS_INIT;

	g_opts = opts;
	if (listen_on(opts->oe_port, session_fiber, "session") != 0 ||
	    listen_on(opts->md_port, md_fiber, "md") != 0 ||
	    listen_on(opts->web_port, web_fiber, "web") != 0)
		return -1;
	if (xtc_loop_init(&g_loop) != XTC_OK)
		return -1;
	po.name = "boot";
	if (xtc_proc_spawn(g_loop, boot_fiber, NULL, &po, &pid) != XTC_OK)
		return -1;
	po.name = "stopper";
	if (xtc_proc_spawn(g_loop, stopper_fiber, NULL, &po, &pid) != XTC_OK)
		return -1;
	if (pthread_create(&g_loop_thread, NULL, loop_thread, NULL) != 0)
		return -1;
	return 0;
}

void
venue_fibers_join(void)
{
	if (g_loop == NULL)
		return;
	(void)pthread_join(g_loop_thread, NULL);
	(void)xtc_loop_fini(g_loop);
	g_loop = NULL;
}
