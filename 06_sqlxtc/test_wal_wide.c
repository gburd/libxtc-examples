/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_wide.c
 *	WAL addresses are 32-bit segment | 32-bit offset (STORAGE_DESIGN
 *	6.2, lead decision on REVIEW-sbf N1), so a log keeps going past
 *	segment 2^24, where the earlier 24-bit field ran out:
 *	  packing    WAL_WA, WAL_WA_SEG and WAL_WA_OFF round-trip segment
 *	             numbers on both sides of 2^24 and up to 2^32 - 1, and
 *	             the largest offset a segment can have;
 *	  rolls      a log whose oldest segment is 2^24 - 2 (written as the
 *	             chain a long-lived store would leave) appends, rolls
 *	             across 2^24 several times, reopens, and a scan from an
 *	             address above 2^24 sees every record with the address
 *	             it was given, in order;
 *	  limits     segment_bytes past what the offset field holds is
 *	             refused; the last segment number refuses to roll.
 *	wal.c is compiled in, for its segment-header writer.
 */

#include "wal.c"

#include <sys/stat.h>

#include "t_tmp.h"

static char g_dir[256];

/* Write segment `seg` with header (first_lsn, base) and no frame: the
 * newest segment a roll leaves (its frames come next). */
static int
make_seg(uint32_t seg, uint64_t first_lsn, uint64_t base)
{
	struct wal_seg_hdr h;
	char p[600];
	int fd, rc;

	wal_seg_path(g_dir, seg, p, sizeof p);
	if ((fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0600)) < 0)
		return -1;
	h.seg = seg;
	h.first_lsn = first_lsn;
	h.base = base;
	rc = wal_seg_hdr_write(fd, &h);
	close(fd);
	return rc;
}

static void
t_packing(void)
{
	static const uint32_t segs[] = { 1, (1u << 24) - 1u, 1u << 24,
	    (1u << 24) + 1u, 0x80000000u, WAL_WA_MAX_SEG };
	uint64_t offs[] = { 0, WAL_SEG_HDR + 12u, WAL_SEG_BYTES_MAX - 1u,
	    WAL_WA_OFF_MASK };
	uint64_t wa, prev = 0;
	size_t i, j;
	int bad = 0;

	for (i = 0; i < sizeof segs / sizeof segs[0]; i++)
		for (j = 0; j < sizeof offs / sizeof offs[0]; j++) {
			wa = WAL_WA(segs[i], offs[j]);
			bad += WAL_WA_SEG(wa) != segs[i] ||
			    WAL_WA_OFF(wa) != offs[j] || wa <= prev;
			prev = wa;
		}
	CK(bad == 0);
	CK(WAL_WA_SEG_BITS == 32 && WAL_WA_OFF_BITS == 32);
	printf("  packing: segment numbers to 2^32 - 1 and offsets to 2^32 - 1 "
	    "round-trip, in order\n");
}

struct acc {
	uint64_t want_lsn, last_wa;
	int n, bad;
	uint64_t *wa;               /* by LSN - first */
	uint64_t first;
};

static int
acc_cb(uint64_t lsn, uint64_t wa, const void *rec, uint32_t len, void *u)
{
	struct acc *a = u;

	(void)rec;
	(void)len;
	if ((a->want_lsn != 0 && lsn != a->want_lsn) || wa <= a->last_wa ||
	    lsn < a->first || a->wa[lsn - a->first] != wa)
		a->bad++;
	a->want_lsn = lsn + 1;
	a->last_wa = wa;
	a->n++;
	return 0;
}

#define NREC 60
#define BASE_SEG ((1u << 24) - 2u)
#define FIRST_LSN 1000000u

static void
t_rolls(void)
{
	static uint64_t was[NREC + 1];
	struct acc a;
	wal_opts_t wo = { 0 };
	uint64_t lsn = 0, wa = 0;
	uint32_t lo, cur;
	wal_t *w = NULL;
	char rec[2000];
	int i;

	/* The chain a long-lived store leaves: retention deleted every
	 * segment below BASE_SEG, which took the next LSN in its header. */
	CK(make_seg(BASE_SEG, FIRST_LSN, 0) == 0);
	wo.path = g_dir;
	wo.append = 1;
	wo.segment_bytes = 16u * 1024u;
	CK(wal_open(&wo, &w) == XTC_OK);
	if (w == NULL)
		return;
	memset(rec, 'w', sizeof rec);
	/* About 5 records of 2 KB per 16 KiB segment: 12 segments. */
	for (i = 0; i < NREC; i++) {
		CK(wal_commit_wa(w, rec, sizeof rec, &lsn, &wa) == XTC_OK);
		was[i] = wa;
		if (i == NREC / 2) {
			wal_close(w);
			w = NULL;
			CK(wal_open(&wo, &w) == XTC_OK);
			if (w == NULL)
				return;
		}
	}
	CK(lsn == FIRST_LSN + NREC - 1u);
	wal_seg_range(w, &lo, &cur);
	CK(lo == BASE_SEG && cur > (1u << 24) + 4u);
	CK(WAL_WA_SEG(was[0]) == BASE_SEG &&
	    WAL_WA_SEG(was[NREC - 1]) == cur);
	wal_close(w);
	/* Every record from one above 2^24, at its own address. */
	for (i = 0; i < NREC && WAL_WA_SEG(was[i]) <= (1u << 24); i++)
		;
	memset(&a, 0, sizeof a);
	a.wa = was + i;
	a.first = FIRST_LSN + (uint64_t)i;
	CK(i < NREC && wal_scan_segs(g_dir, was[i], acc_cb, &a) == XTC_OK);
	CK(a.n == NREC - i && a.bad == 0);
	printf("  rolls: segments %u..%u (across 2^24), a reopen in between; a "
	    "scan from segment %u sees all %d records at their addresses\n",
	    lo, cur, WAL_WA_SEG(was[i]), a.n);
}

static void
t_limits(void)
{
	wal_opts_t wo = { 0 };
	uint64_t lsn, wa;
	char sub[300];
	wal_t *w = NULL;

	snprintf(sub, sizeof sub, "%s/lim", g_dir);
	wo.path = sub;
	wo.segment_bytes = WAL_SEG_BYTES_MAX + 1u;
	CK(wal_open(&wo, &w) == XTC_E_INVAL && w == NULL);
	wo.segment_bytes = 4ull << 30;
	CK(wal_open(&wo, &w) == XTC_E_INVAL && w == NULL);
	/* The last segment number: a roll fail-stops the log (there is no
	 * next segment), and nothing more commits. */
	CK(mkdir(sub, 0700) == 0);
	snprintf(g_dir + strlen(g_dir), sizeof g_dir - strlen(g_dir), "/lim");
	CK(make_seg(WAL_WA_MAX_SEG, 7, 0) == 0);
	wo.append = 1;
	wo.segment_bytes = 16u * 1024u;
	CK(wal_open(&wo, &w) == XTC_OK);
	if (w == NULL)
		return;
	CK(wal_commit_wa(w, "x", 1, &lsn, &wa) == XTC_OK &&
	    WAL_WA_SEG(wa) == WAL_WA_MAX_SEG && lsn == 7);
	/* A record whose trailer would end past offset 2^32 - 1 cannot be
	 * addressed (checked before the record is read). */
	CK(wal_commit_wa(w, "z", (uint32_t)(WAL_WA_OFF_MASK + 1u -
	    WAL_SEG_HDR - 12u - 8u + 1u), &lsn, &wa) == XTC_E_INVAL);
	CK(wal_roll(w) == XTC_E_IO && wal_failed(w));
	CK(wal_commit_wa(w, "y", 1, &lsn, &wa) != XTC_OK);
	wal_close(w);
	printf("  limits: segment_bytes past the offset field refused; a frame "
	    "ending past 2^32 refused; segment 2^32 - 1 never rolls\n");
}

/* Remove the files in d (and d/lim), then d. */
static void
cleanup(const char *d)
{
	char p[600];
	DIR *dp;
	struct dirent *e;

	if ((dp = opendir(d)) == NULL)
		return;
	while ((e = readdir(dp)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(p, sizeof p, "%s/%s", d, e->d_name);
		if (unlink(p) != 0)
			cleanup(p);
	}
	closedir(dp);
	(void)rmdir(d);
}

int
main(void)
{
	char top[256];

	t_tmpl(g_dir, sizeof g_dir, "walwide");
	if (mkdtemp(g_dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(top, sizeof top, "%s", g_dir);
	t_packing();
	t_rolls();
	t_limits();
	cleanup(top);
	if (!g_fail)
		printf("ok test_wal_wide\n");
	return g_fail;
}
