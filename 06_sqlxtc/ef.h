/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/ef.h
 *	Elias-Fano chunks: the static stage of the row log's keydir
 *	(STORAGE_DESIGN 5.2).  A chunk encodes up to EF_CHUNK strictly
 *	increasing rowids of one table in one run, plus the page map that
 *	turns a rowid's rank in the chunk into the run page holding it.
 *	Each chunk is self-contained, so a lookup reads one chunk.
 *
 *	Encoding (Vigna, "Quasi-Succinct Indices", 3): the rowids, mapped
 *	to unsigned order (sign bit flipped) and taken relative to the
 *	first, split into l low bits stored plainly and high bits stored
 *	as unary gaps, l = floor(log2(span / n)).  That is at most about
 *	2 + l bits per rowid.  The page map is the page of the chunk's
 *	first row and then, for each later page, the rank at which it
 *	starts (2 bytes per page).
 *
 *	Layout, all integers little-endian:
 *	    [0, 8)    base: the first rowid, sign bit flipped
 *	    [8, 10)   n, the number of rowids (1..EF_CHUNK)
 *	    [10]      l, the low-bit width (0..63)
 *	    [11]      0
 *	    [12, 14)  uw, the upper bits' length in 64-bit words
 *	    [14, 16)  np, page starts after the first page
 *	    [16, 20)  the first row's page
 *	    then uw words of upper bits, ceil(n * l / 64) words of low
 *	    bits, and np u16 page starts, each (rank:9 | skip:7): the rank
 *	    (1..n-1, ascending) at which the next data page starts, and
 *	    the number of other pages (a long row's continuation pages)
 *	    between it and the data page before.  The page of rank r is
 *	    the first page plus 1 + skip for every start <= r.
 */

#ifndef SQLXTC_EF_H
#define SQLXTC_EF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EF_CHUNK   512u     /* rowids per chunk (the last may hold fewer) */
#define EF_HDR     20u

/*
 * Bytes a chunk of these n rowids needs (n in 1..EF_CHUNK), with np page
 * starts.  rid[] must be strictly increasing.
 */
size_t ef_chunk_bytes(const int64_t *rid, uint32_t n, uint32_t np);

/*
 * Encode n rowids into buf (cap bytes).  page[i] is the run page of row
 * i: non-decreasing, and a step skips at most EF_MAX_SKIP pages.
 * Returns the bytes written, or 0 if the input is invalid (n out of
 * range, rid not strictly increasing, a page step too large) or buf is
 * too small.
 */
#define EF_MAX_SKIP 127u
size_t ef_chunk_build(const int64_t *rid, const uint32_t *page, uint32_t n,
    uint8_t *buf, size_t cap);

/*
 * Check that buf holds a well-formed chunk of len bytes (header fields
 * consistent with len, page starts ascending).  1 if so.  Run readers
 * call it once per chunk they load, so a damaged footer is an error,
 * never a wild read.
 */
int ef_chunk_valid(const uint8_t *buf, size_t len);

/* The number of rowids, and the first and last rowid, of a valid chunk. */
uint32_t ef_chunk_count(const uint8_t *buf);
int64_t  ef_chunk_first(const uint8_t *buf);
int64_t  ef_chunk_last(const uint8_t *buf);

/*
 * Find rid in a valid chunk: 1 with *rank (its position, 0-based) and
 * *page (its run page) if present, 0 if absent.  Either out pointer may
 * be NULL.
 */
int ef_chunk_find(const uint8_t *buf, int64_t rid, uint32_t *rank,
    uint32_t *page);

/* The rowid at rank r (< count) of a valid chunk, and its page. */
int64_t  ef_chunk_get(const uint8_t *buf, uint32_t r);
uint32_t ef_chunk_page(const uint8_t *buf, uint32_t r);

/* Decode every rowid of a valid chunk into out[count] (a scan). */
void ef_chunk_decode(const uint8_t *buf, int64_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SQLXTC_EF_H */
