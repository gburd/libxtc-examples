/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_bm_files.c
 *	Multi-file page ids in the buffer manager (bm_file_attach): a page
 *	id is (file:16, page:48), file 0 is the page file.  The cases:
 *	  same page      page 3 of files 0, 1 and 2 are three pages, cached
 *	                 and reloaded apart (the hash, do_io's offset);
 *	  file seal      a page sealed for file 1 written into file 2 at
 *	                 the same offset is refused, as is a file-1 page
 *	                 read back as page 3 of the page file;
 *	  file 0 seal    a file-0 page's seal is still the CRC of the page
 *	                 with its CRC word as zero (the format is unchanged);
 *	  writeback      a dirtied file-1 page that is evicted is written to
 *	                 file 1 at its own offset, never to the page file;
 *	  detach         drops the file's frames unwritten; a fix of it then
 *	                 fails, and another file attached under the same
 *	                 number serves its own bytes, not stale frames;
 *	  double-write   a torn file-1 page is repaired on reopen when the
 *	                 file is attached (the ring keeps the image);
 *	  freelist       bm_free_pid refuses a page of another file.
 *	Runs off a loop: xtc_aio is inline there.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bufmgr.h"
#include "btnode.h"
#include "wal.h"
#include "xtc.h"
#include "t_tmp.h"

#define PS      4096
#define NPG     8

static char g_dir[256];

/* A file path in the test directory. */
static void
path_of(char *out, size_t cap, const char *name)
{
	snprintf(out, cap, "%s/%s", g_dir, name);
}

/* Page contents: a tag, the page id, a fill byte; the checksum slot
 * (BTNODE_CKSUM_OFF) is left for the seal. */
static void
fill(uint8_t *p, bm_pid_t pid, int tag)
{
	memset(p, 0, PS);
	p[0] = (uint8_t)tag;
	memcpy(p + 8, &pid, 8);
	memset(p + 64, tag, PS - 64);
}

static int
check(const uint8_t *p, bm_pid_t pid, int tag)
{
	bm_pid_t got;
	memcpy(&got, p + 8, 8);
	return p[0] == (uint8_t)tag && got == pid && p[PS - 1] == (uint8_t)tag;
}

/* Write NPG sealed pages of file `file` (tag file*16+page) to `path`. */
static void
make_file(bm_t *bm, const char *path, uint16_t file)
{
	uint8_t pg[PS];
	int fd, i;

	CK((fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600)) >= 0);
	for (i = 0; fd >= 0 && i < NPG; i++) {
		bm_pid_t pid = BM_PID_MAKE(file, i);
		fill(pg, pid, file * 16 + i);
		bm_seal_page(bm, pg, pid);
		CK(pwrite(fd, pg, PS, (off_t)i * PS) == PS);
	}
	if (fd >= 0)
		close(fd);
}

static int
fix_check(bm_t *bm, bm_pid_t pid, int tag)
{
	bm_frame_t *f;
	int ok;

	if (bm_fix_pid(bm, pid, &f) != XTC_OK)
		return 0;
	ok = check(bm_page(f), pid, tag);
	bm_unfix(bm, f, 0);
	return ok;
}

static bm_t *
open_bm(const char *pfile, int reopen, int dw, uint32_t frames)
{
	bm_opts_t o = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;

	o.path = pfile;
	o.page_size = PS;
	o.n_frames = frames;
	o.reopen = (uint8_t)reopen;
	o.double_write = (uint8_t)dw;
	o.cksum_off = BTNODE_CKSUM_OFF;
	o.cksum = wal_crc32c;
	CK(bm_create(&o, &bm) == XTC_OK);
	return bm;
}

static void
t_same_page(void)
{
	char p0[300], p1[300], p2[300];
	bm_frame_t *f;
	bm_pid_t pid;
	bm_t *bm;
	int i;

	path_of(p0, sizeof p0, "pages");
	path_of(p1, sizeof p1, "run1");
	path_of(p2, sizeof p2, "run2");
	bm = open_bm(p0, 0, 0, 16);
	if (bm == NULL)
		return;
	make_file(bm, p1, 1);
	make_file(bm, p2, 2);
	CK(bm_file_attach(bm, 1, p1) == XTC_OK);
	CK(bm_file_attach(bm, 2, p2) == XTC_OK);
	CK(bm_file_attach(bm, 2, p2) == XTC_E_INVAL);   /* already attached */
	CK(bm_file_attach(bm, 0, p2) == XTC_E_INVAL);
	/* Page-file pages 1..NPG-1, tagged 200+page. */
	for (i = 1; i < NPG; i++) {
		CK(bm_alloc_pid(bm, &f, &pid) == XTC_OK);
		CK(pid == (bm_pid_t)i);
		fill(bm_page(f), pid, 200 + i);
		bm_unfix(bm, f, 1);
	}
	CK(bm_checkpoint(bm) == XTC_OK);
	/* 16 frames, 3 x 7 pages: every round reloads most of them. */
	for (i = 0; i < 3 * NPG; i++) {
		int pg = 1 + i % (NPG - 1);
		CK(fix_check(bm, BM_PID_MAKE(0, pg), 200 + pg));
		CK(fix_check(bm, BM_PID_MAKE(1, pg), 16 + pg));
		CK(fix_check(bm, BM_PID_MAKE(2, pg), 32 + pg));
	}
	CK(bm_dbg_dup_pid(bm) == 0);
	CK(bm_free_pid(bm, BM_PID_MAKE(1, 3)) == XTC_E_INVAL);
	CK(bm_cksum_failures(bm, NULL) == 0);
	bm_destroy(bm);
	printf("  same page: page n of files 0, 1, 2 cached and reloaded "
	    "apart\n");
}

static void
t_file_seal(void)
{
	char p0[300], p1[300], p2[300];
	uint8_t pg[PS], z[4] = { 0 };
	bm_frame_t *f;
	bm_pid_t bad = 0;
	uint32_t c, slot;
	bm_t *bm;
	int fd;

	path_of(p0, sizeof p0, "pages");
	path_of(p1, sizeof p1, "run1");
	path_of(p2, sizeof p2, "run2");
	bm = open_bm(p0, 0, 0, 16);
	if (bm == NULL)
		return;
	make_file(bm, p1, 1);
	make_file(bm, p2, 2);
	/* Page 3 of file 1, intact, written over page 3 of file 2: the same
	 * page number and offset, only the file number differs. */
	CK((fd = open(p1, O_RDONLY)) >= 0);
	CK(pread(fd, pg, PS, 3 * PS) == PS);
	close(fd);
	CK((fd = open(p2, O_RDWR)) >= 0);
	CK(pwrite(fd, pg, PS, 3 * PS) == PS);
	close(fd);
	/* ... and over page 3 of the page file. */
	CK((fd = open(p0, O_RDWR)) >= 0);
	CK(pwrite(fd, pg, PS, 3 * PS) == PS);
	close(fd);
	CK(bm_file_attach(bm, 1, p1) == XTC_OK);
	CK(bm_file_attach(bm, 2, p2) == XTC_OK);
	CK(fix_check(bm, BM_PID_MAKE(1, 3), 16 + 3));
	CK(bm_fix_pid(bm, BM_PID_MAKE(2, 3), &f) == XTC_E_IO);
	CK(bm_cksum_failures(bm, &bad) == 1 && bad == BM_PID_MAKE(2, 3));
	CK(bm_fix_pid(bm, BM_PID_MAKE(0, 3), &f) == XTC_E_IO);
	CK(bm_cksum_failures(bm, &bad) == 2 && bad == BM_PID_MAKE(0, 3));
	/* A file-0 seal is the plain CRC32C of the page with its CRC word
	 * as zero, and the pid word: the page file's format is unchanged. */
	fill(pg, 5, 77);
	bm_seal_page(bm, pg, 5);
	memcpy(&slot, pg + BTNODE_CKSUM_OFF, 4);
	c = wal_crc32c(0, pg, BTNODE_CKSUM_OFF);
	c = wal_crc32c(c, z, 4);
	c = wal_crc32c(c, pg + BTNODE_CKSUM_OFF + 4, PS - BTNODE_CKSUM_OFF - 4);
	CK(slot == c);
	CK(pg[BTNODE_CKSUM_OFF + 4] == 5 && pg[BTNODE_CKSUM_OFF + 5] == 0);
	bm_destroy(bm);
	printf("  file seal: a page of file 1 refused as file 2's and as the "
	    "page file's; the file-0 seal is unchanged\n");
}

static void
t_writeback_detach(void)
{
	char p0[300], p1[300], p3[300];
	uint8_t pg[PS];
	bm_frame_t *f;
	bm_pid_t pid;
	bm_t *bm;
	int fd, i;

	path_of(p0, sizeof p0, "pages");
	path_of(p1, sizeof p1, "run1");
	path_of(p3, sizeof p3, "run3");
	bm = open_bm(p0, 0, 0, 4);
	if (bm == NULL)
		return;
	make_file(bm, p1, 1);
	make_file(bm, p3, 3);
	CK(bm_file_attach(bm, 1, p1) == XTC_OK);
	CK(bm_fix_pid(bm, BM_PID_MAKE(1, 2), &f) == XTC_OK);
	fill(bm_page(f), BM_PID_MAKE(1, 2), 99);
	bm_unfix(bm, f, 1);
	/* Churn the 4-frame pool so the dirty page is written and evicted. */
	for (i = 0; i < 12; i++) {
		CK(bm_alloc_pid(bm, &f, &pid) == XTC_OK);
		fill(bm_page(f), pid, 100);
		bm_unfix(bm, f, 1);
	}
	CK(bm_checkpoint(bm) == XTC_OK);
	CK((fd = open(p1, O_RDONLY)) >= 0);
	CK(pread(fd, pg, PS, 2 * PS) == PS);
	close(fd);
	CK(check(pg, BM_PID_MAKE(1, 2), 99));
	CK(fix_check(bm, BM_PID_MAKE(1, 2), 99));
	/* Detach with the page resident and dirty again: dropped unwritten. */
	CK(bm_fix_pid(bm, BM_PID_MAKE(1, 2), &f) == XTC_OK);
	fill(bm_page(f), BM_PID_MAKE(1, 2), 98);
	bm_unfix(bm, f, 1);
	CK(bm_file_detach(bm, 1) == XTC_OK);
	CK(bm_file_detach(bm, 1) == XTC_E_INVAL);
	CK(bm_fix_pid(bm, BM_PID_MAKE(1, 2), &f) == XTC_E_INVAL);
	CK(bm_checkpoint(bm) == XTC_OK);
	CK((fd = open(p1, O_RDONLY)) >= 0);
	CK(pread(fd, pg, PS, 2 * PS) == PS);
	close(fd);
	CK(check(pg, BM_PID_MAKE(1, 2), 99));
	/* Another file under number 1: its pages were sealed as file 3's,
	 * so a stale frame of the old file 1 would pass, a reload fails. */
	CK(bm_file_attach(bm, 1, p3) == XTC_OK);
	CK(bm_fix_pid(bm, BM_PID_MAKE(1, 2), &f) == XTC_E_IO);
	CK(bm_dbg_dup_pid(bm) == 0);
	bm_destroy(bm);
	printf("  writeback/detach: an evicted file-1 page lands in file 1; "
	    "detach drops frames unwritten\n");
}

static void
t_double_write(void)
{
	char p0[300], p1[300];
	uint8_t pg[PS];
	bm_frame_t *f;
	bm_stats_t st;
	bm_t *bm;
	int fd;

	path_of(p0, sizeof p0, "dwpages");
	path_of(p1, sizeof p1, "dwrun1");
	bm = open_bm(p0, 0, 1, 16);
	if (bm == NULL)
		return;
	make_file(bm, p1, 1);
	CK(bm_file_attach(bm, 1, p1) == XTC_OK);
	CK(bm_fix_pid(bm, BM_PID_MAKE(1, 4), &f) == XTC_OK);
	fill(bm_page(f), BM_PID_MAKE(1, 4), 55);
	bm_unfix(bm, f, 1);
	CK(bm_checkpoint(bm) == XTC_OK);
	bm_destroy(bm);
	/* Tear the home write: half of the page is the old image. */
	CK((fd = open(p1, O_RDWR)) >= 0);
	memset(pg, 0xEE, PS / 2);
	CK(pwrite(fd, pg, PS / 2, 4 * PS + PS / 2) == PS / 2);
	close(fd);
	bm = open_bm(p0, 1, 1, 16);
	if (bm == NULL)
		return;
	CK(bm_file_attach(bm, 1, p1) == XTC_OK);
	CK(fix_check(bm, BM_PID_MAKE(1, 4), 55));
	CK(fix_check(bm, BM_PID_MAKE(1, 5), 16 + 5));
	bm_get_stats(bm, &st);
	CK(st.dw_repaired == 1);
	CK(st.cksum_failures == 0);
	bm_destroy(bm);
	printf("  double-write: a torn file-1 page repaired at attach\n");
}

int
main(void)
{
	static const char *names[] = { "pages", "pages.dwb", "run1", "run2",
	    "run3", "dwpages", "dwpages.dwb", "dwrun1" };
	char p[300];
	size_t i;

	t_tmpl(g_dir, sizeof g_dir, "bmfiles");
	if (mkdtemp(g_dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	t_same_page();
	t_file_seal();
	t_writeback_detach();
	t_double_write();
	for (i = 0; i < sizeof names / sizeof names[0]; i++) {
		path_of(p, sizeof p, names[i]);
		(void)unlink(p);
	}
	(void)rmdir(g_dir);
	if (!g_fail)
		printf("ok test_bm_files\n");
	return g_fail;
}
