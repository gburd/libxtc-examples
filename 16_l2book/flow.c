/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * 16_l2book/flow.c -- l2book-flow: an outside order-flow client.
 *
 *	Connects to a venue's order-entry port, logs in, and sends a noise
 *	flow (resting orders near the touch, marketable IOCs, cancels) at a
 *	fixed rate per symbol, reading every report back.  Plain blocking
 *	sockets and one thread: it is a client, not part of the venue.  It
 *	learns the touch from its own fills and rests around the reference
 *	price.  Reconnects if the venue restarts.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int
dial(const char *hostport)
{
	char host[256];
	const char *colon = strrchr(hostport, ':');
	struct addrinfo hints, *res;
	int fd, one = 1;
	if (colon == NULL || (size_t)(colon - hostport) >= sizeof host)
		return -1;
	memcpy(host, hostport, (size_t)(colon - hostport));
	host[colon - hostport] = '\0';
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, colon + 1, &hints, &res) != 0)
		return -1;
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd >= 0)
		(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	return fd;
}

static int
send_rec(int fd, const unsigned char *r, uint32_t n)
{
	unsigned char b[300];
	size_t off = 0;
	b[0] = (unsigned char)n;
	b[1] = (unsigned char)(n >> 8);
	b[2] = b[3] = 0;
	memcpy(b + 4, r, n);
	while (off < 4 + n) {
		ssize_t w = write(fd, b + off, 4 + n - off);
		if (w <= 0)
			return -1;
		off += (size_t)w;
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
xs(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

/* Read and discard whatever reports are waiting (we only count them). */
static long
drain(int fd)
{
	unsigned char buf[65536];
	long n = 0;
	struct pollfd p = { fd, POLLIN, 0 };
	while (poll(&p, 1, 0) > 0) {
		ssize_t r = read(fd, buf, sizeof buf);
		if (r <= 0)
			return -1;
		n += r;
	}
	return n;
}

int
main(int argc, char **argv)
{
	static const struct option lo[] = {
		{ "venue", required_argument, NULL, 'v' },
		{ "rate", required_argument, NULL, 'r' },
		{ "symbols", required_argument, NULL, 's' },
		{ "ref", required_argument, NULL, 'p' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	const char *venue = "127.0.0.1:9000";
	double rate = 100;
	int nsym = 8, c;
	int64_t ref = 10000;
	uint64_t rng = (uint64_t)getpid() * 0x9e3779b97f4a7c15ull + 1, cid = 1;

	while ((c = getopt_long(argc, argv, "v:r:s:p:h", lo, NULL)) != -1) {
		switch (c) {
		case 'v': venue = optarg; break;
		case 'r': rate = atof(optarg); break;
		case 's': nsym = atoi(optarg); break;
		case 'p': ref = atoll(optarg); break;
		case 'h':
			printf("usage: l2book-flow [--venue HOST:PORT] [--rate R] "
			    "[--symbols N] [--ref TICKS]\n\nAn outside order-flow "
			    "client for l2book: noise orders over the order-entry "
			    "protocol.\n");
			return 0;
		default:
			return 2;
		}
	}
	for (;;) {
		int fd = dial(venue);
		unsigned char r[80];
		struct timespec ts;
		long sent = 0;
		if (fd < 0) {
			fprintf(stderr, "l2book-flow: %s: %s; retrying\n", venue,
			    strerror(errno));
			sleep(1);
			continue;
		}
		memset(r, 0, sizeof r);
		r[0] = 'L';	/* account 0: the venue assigns one */
		if (send_rec(fd, r, 21) != 0) {
			close(fd);
			continue;
		}
		fprintf(stderr, "l2book-flow: connected to %s, %.0f orders/s\n",
		    venue, rate);
		ts.tv_sec = 0;
		ts.tv_nsec = (long)(1e9 / (rate > 0 ? rate : 1));
		for (;;) {
			uint64_t x = xs(&rng);
			uint16_t sym = (uint16_t)(x % (uint64_t)nsym);
			int side = (int)((x >> 8) & 1), ioc = (x >> 9) % 4 == 0;
			unsigned char *p = r;
			int64_t px = ioc ? (side ? ref - 30 : ref + 30) :
			    (side ? ref + 1 + (int64_t)((x >> 12) % 8) :
			    ref - 1 - (int64_t)((x >> 12) % 8));
			memset(r, 0, sizeof r);
			*p++ = 'O';
			p = p64(p, cid++);
			*p++ = (unsigned char)sym;
			*p++ = 0;
			*p++ = (unsigned char)side;
			*p++ = 0;			/* limit */
			*p++ = ioc ? 1 : 3;		/* IOC or DAY */
			p += 3;
			p = p64(p, (uint64_t)px);
			p = p64(p, 0);
			p = p64(p, 100 * (1 + (x >> 20) % 3));
			if (send_rec(fd, r, 1 + 8 + 2 + 6 + 5 * 8 + 16) != 0 ||
			    drain(fd) < 0)
				break;
			if (cid > 64 && (x >> 24) % 2 == 0) {
				memset(r, 0, sizeof r);
				r[0] = 'X';
				p64(r + 1, cid - 1 - (x >> 28) % 32);
				r[9] = (unsigned char)sym;
				if (send_rec(fd, r, 11) != 0)
					break;
			}
			if (++sent % 10000 == 0)
				fprintf(stderr, "l2book-flow: %ld orders sent\n", sent);
			nanosleep(&ts, NULL);
		}
		close(fd);
		fprintf(stderr, "l2book-flow: venue went away; reconnecting\n");
		sleep(1);
	}
}
