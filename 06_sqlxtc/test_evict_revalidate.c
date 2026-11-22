/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * Force a writer between eviction's prechecks and reservation. Include the
 * implementation to control the interleaving without exporting a test API.
 */
#include "bufmgr.h"
static void race_hook(bm_t *, bm_frame_t *, int);
#define BM_EVICT_TEST_HOOK(bm, f, phase) race_hook(bm, f, phase)
#include "bufmgr.c"
#include "t_tmp.h"

static bm_frame_t *target;
static int armed, staged;

static void
race_hook(bm_t *bm, bm_frame_t *f, int phase)
{
	bm_frame_t *writer;
	if (!armed || f != target)
		return;
	if (phase == 2) {
		if (bm_fix_pid(bm, bm_frame_pid(f), &writer) != XTC_OK || writer != f)
			abort();
		bm_latch_exclusive(f);
		((unsigned char *)bm_page(f))[0] = 0x72;
		bm_unlatch(f);
		bm_unfix(bm, f, 1);
		/* Sampling already saw this incarnation COOL: emulate a
		 * concurrent provider's harmless cooling after the write. */
		xtc_atomic_store_u8(&f->state, BM_COOL, xtc_mo_seq_cst);
		staged = 1;
		armed = 0;
		return;
	}
	if (phase == 0 && bm->claim_threshold != 0 && !staged &&
	    xtc_atomic_load_u32(&bm->clock, xtc_mo_seq_cst) ==
	        (uint32_t)(f - bm->frames) + 1)
		return;
	if (phase == 0 && !staged) {
		/* Evictor has already seen pin==0 and HOT. The writer pins
		 * while still HOT, so its fix does not change the state. */
		if (bm_fix_pid(bm, bm_frame_pid(f), &writer) != XTC_OK || writer != f)
			abort();
		staged = 1;
	} else if (phase == 1 && staged) {
		/* Evictor has demoted HOT->COOL and checked dirty/io/ref.
		 * Complete the write before its CAS(pin 0 -> -1). */
		bm_latch_exclusive(f);
		((unsigned char *)bm_page(f))[0] = 0x72;
		bm_unlatch(f);
		bm_unfix(bm, f, 1);
		armed = 0;
	}
}

#ifdef BM_SAMPLED_EVICT
/* The evict_seed whose 16 draws all miss frame 3 of a 4-frame pool: the
 * frame this test's one page lands in (the free list hands out the last
 * frame pushed).  The sample then finds no COOL frame, and only the
 * sweep after it can reclaim the page. */
#define MISS_SEED  38u
#define MISS_FRAME 3u

/* Nonzero if the sampler, seeded with `seed`, never draws frame `idx`. */
static int
sample_misses(uint64_t seed, uint32_t idx, uint32_t n)
{
	unsigned d;

	for (d = 0; d < BM_D_SAMPLE; d++) {
		seed = seed * 6364136223846793005ull + 1442695040888963407ull;
		if ((uint32_t)((seed >> 33) % n) == idx)
			return 0;
	}
	return 1;
}
#endif

static int
run(uint32_t theta, uint64_t seed)
{
	bm_opts_t opts = BM_OPTS_DEFAULT;
	bm_t *bm;
	bm_frame_t *f;
	bm_pid_t pid;
	char path[256];
	int fd, attempt, reclaimed = 0, rc = 1;
	unsigned char got;

	t_tmpl(path, sizeof path, "sqlxtc-evict-revalidate");
	fd = mkstemp(path);
	if (fd < 0)
		return 1;
	close(fd);
	opts.path = path;
	opts.n_frames = 4;
	opts.scan_resist = theta != 0; /* claim arm first passes HOT */
	opts.claim_threshold = theta;
	opts.evict_seed = seed;
	if (bm_create(&opts, &bm) != XTC_OK)
		goto unlink_out;
	if (bm_alloc_pid(bm, &f, &pid) != XTC_OK)
		goto out;
	((unsigned char *)bm_page(f))[0] = 0x31;
	bm_unfix(bm, f, 1);
	if (bm_checkpoint(bm) != XTC_OK)
		goto out;
	target = f;
#ifdef BM_SAMPLED_EVICT
	if (seed != 0 && ((uint32_t)(f - bm->frames) != MISS_FRAME ||
	    !sample_misses(seed, MISS_FRAME, opts.n_frames))) {
		fprintf(stderr, "FAIL: seed %llu does not miss the page's frame "
		    "%u\n", (unsigned long long)seed, (unsigned)(f - bm->frames));
		goto out;
	}
#endif
	xtc_atomic_store_i32(&f->ref, 0, xtc_mo_seq_cst);
	xtc_atomic_store_u32(&bm->clock, (uint32_t)(f - bm->frames),
	    xtc_mo_seq_cst);
	/* Claim arm spends its budget passing over HOT, then reaches this
	 * same frame on the next cycle. Don't start the writer on that pass. */
	armed = 1;
	staged = 0;
#ifdef BM_SAMPLED_EVICT
	xtc_atomic_store_u8(&f->state, BM_COOL, xtc_mo_seq_cst);
	reclaimed = sample_evict_one(bm);
	/* Flush/reclaim after the forced interleaving if correctly spared. */
#endif
	/* A raced dirty frame can require writeback followed by a retry,
	 * just as get_free_frame retries evict_one. */
	for (attempt = 0; attempt < 4 && !reclaimed; attempt++)
		reclaimed = evict_one(bm);
	if (!reclaimed || armed || !staged) {
		fprintf(stderr, "FAIL: interleaving did not reclaim: theta=%u "
		    "seed=%llu reclaimed=%d armed=%d staged=%d\n", theta,
		    (unsigned long long)seed, reclaimed, armed, staged);
		goto out;
	}
	if (bm_fix_pid(bm, pid, &f) != XTC_OK)
		goto out;
	got = ((unsigned char *)bm_page(f))[0];
	bm_unfix(bm, f, 0);
	if (got != 0x72) {
		fprintf(stderr, "FAIL: theta=%u eviction lost completed write: "
		    "got 0x%02x, expected 0x72\n", theta, got);
		goto out;
	}
	rc = 0;
out:
	bm_destroy(bm);
unlink_out:
	unlink(path);
	return rc;
}

int
main(void)
{
	int a = run(0, 0), b = run(1, 0), c = 0;
#ifdef BM_SAMPLED_EVICT
	/* The sample misses the only COOL frame: the sweep must find it. */
	c = run(1, MISS_SEED);
#endif
	if (a || b || c)
		return 1;
	puts("OK: eviction revalidates a completed write under reservation");
	return 0;
}
