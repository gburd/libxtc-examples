/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/xstore.c
 *	A SQLite virtual-table module that runs SQL on the libxtc-native
 *	storage engine (btree.c over the cooling buffer pool, bufmgr.c)
 *	with multi-version concurrency control (MVCC) snapshot reads.
 *
 *	SQLite tokenizes/parses/plans/runs the VDBE; table I/O is
 *	redirected here, into our on-disk, larger-than-RAM-capable
 *	B-tree.  Reimplementing the amalgamation's internal btree.h is
 *	impractical; the virtual-table seam is the supported mechanism.
 *
 *	MVCC model (see
 *	the design notes).  Each row VERSION is stored under the
 *	B-tree key (rowid, commit_ts), with versions of one rowid
 *	clustered and ordered newest-first (the timestamp half is stored
 *	bit-inverted so a forward scan visits the highest commit_ts
 *	first).  The version's commit_ts is its xmin (the transaction
 *	that created it); a delete writes a tombstone version.  A read at
 *	snapshot S returns, per rowid, the newest version with
 *	commit_ts <= S that is not a tombstone; newer
 *	versions stand in for xmax.
 *	Readers never block writers and writers never block readers.
 *
 *	A single global logical clock supplies commit timestamps and
 *	snapshots for this one shared B-tree (the per-shard hybrid
 *	logical clock in mvcc.c is the sharded variant).  Two SQL
 *	functions expose it: xstore_now() returns the current clock, and
 *	xstore_as_of(ts) pins this connection's reads at snapshot ts (0 =
 *	latest) -- a time-travel / AS OF read that makes snapshot
 *	visibility observable from SQL.
 *
 *	STATUS: implemented.  Transaction-level snapshot isolation spans
 *	multiple statements -- xBegin captures one read snapshot, writes
 *	buffer, and xCommit assigns a single commit timestamp and flushes
 *	the whole transaction (so all of its rows share one xmin); a
 *	multi-statement BEGIN..COMMIT (or separate Quack messages on the
 *	same handle) forms one transaction.  Cahill SSI serializability is
 *	enforced: xs_sync (2PC phase 1) calls ssi_in_conflict and returns
 *	SX_BUSY on a dangerous rw-antidependency pivot, rolling the
 *	transaction back -- with predicate (range) locking so disjoint
 *	readers do not falsely conflict.  See the design notes and
 *	the scenario_serializable / SSI-range tests in test_xstore.c.
 */

#include "engine.h"      /* SX_* result codes */
#include "xstore.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>        /* signbit: the record codec keeps -0.0 */
#include <string.h>
#include <strings.h>     /* strcasecmp: SQL table names are case-insensitive */

#include "btree.h"
#include "wal.h"
#include "xlog.h"
#include "xtc.h"
#include "xtc_blocking.h"
#include "xtc_io.h"      /* xtc_io_backend_name: no pool threads under sim */
#include "xtc_proc.h"
#include "xtc_sync.h"
#include "sx_dst.h"       /* DST planted bugs (no-op in prod) */
#include "sx_siphash.h"   /* the hash of a truncated index key */
#include "xtc_sim.h"      /* XTC_SIM_BUGGIFY: DST pessimal points (no-op in prod) */

/*
 * Process-global engine locks.  These were pthread mutexes; they are
 * now xtc_amutex parking mutexes (xtc_amutex_static slots), so a holder
 * may PARK on I/O (e.g. xs_put waiting on the WAL ack) inside the
 * critical section without wedging a cooperative loop when another
 * fiber on that loop contends.  That is what lets the catalog persist
 * its new row inside the lock, closing the cross-connection
 * CREATE-then-SELECT race by construction.  Each name maps to a stable
 * static slot; xtc_amutex_static lazily creates and never frees it.
 */
#define XS_LK_CAT  8u   /* catalog (g_cat) + view registry */
#define XS_LK_SSI  9u   /* SSI registry */
/* slot 10 (XS_LK_SNAP, the snapshot registry) retired: lock-free now */
#define XS_LK_COMMIT 11u /* commit validation + timestamp + in-flight registry */
#define XS_LK_DDL  12u  /* one DDL statement at a time (xstore_ddl_lock) */
static uint64_t g_cat_locks; /* XS_LK_CAT acquisitions (metric) */
static uint64_t g_cat_scans; /* catalog-tree scans (metric) */
/*
 * Every hold runs with cancellation masked: a holder parks (a B-tree
 * latch, the WAL ack) and a kill delivered there would unwind with the
 * lock still held -- nothing releases an amutex for a dead proc, so
 * every later caller would wait forever (test_wal_kill_engine).  The
 * kill is delivered at the unlock instead.  No-ops off a proc.
 */
static xtc_amutex_t *xs_lk_get(unsigned slot);
static void xs_lk(unsigned slot);
static void xs_unlk(unsigned slot);
static void xs_commit_unlock(void);
#define xs_cat_lock()                                                          \
	(xtc_atomic_fetch_add_u64(&g_cat_locks, 1, xtc_mo_relaxed),            \
	    xs_lk(XS_LK_CAT))
#define xs_cat_unlock()  xs_unlk(XS_LK_CAT)
#define xs_ssi_lock()    xs_lk(XS_LK_SSI)
#define xs_ssi_unlock()  xs_unlk(XS_LK_SSI)
#define xs_commit_lock()   xs_lk(XS_LK_COMMIT)

/*
 * Commit clock.  Every committed version is stamped with a unique,
 * monotonically increasing 64-bit timestamp that is also the MVCC
 * snapshot boundary: a snapshot S sees exactly the versions whose
 * commit_ts <= S.  The stamp is a Hybrid Logical Clock (HLC):
 *
 *   bits 63..16  physical time in milliseconds since the epoch
 *   bits 15..0   logical counter (disambiguates same-millisecond
 *                commits and guarantees strict monotonicity)
 *
 * Packing physical time into the high bits keeps the stamp ordered as
 * a plain uint64_t -- so every existing commit_ts comparison, the
 * bit-inverted version-key encoding, and the persisted clock keep
 * working unchanged -- while anchoring it to wall-clock time, so a
 * snapshot is meaningful across nodes and the clock survives a
 * restart monotonically (it is persisted and restored, and never
 * runs backwards even if two nodes' wall clocks disagree, because the
 * HLC update rule advances past any observed timestamp).  Physical
 * milliseconds keep real stamps far below the steal-txn range
 * (1<<62), so the two id spaces never collide.
 *
 * In a single process this is still one shared atomic advanced by CAS
 * per commit; HLC's payoff is the wall-clock anchoring and the
 * merge-on-observe rule (xstore_clock_observe) that make the same
 * stamp valid as a causal timestamp once the engine is distributed.
 */
#define HLC_LOGICAL_BITS 16
#define HLC_LOGICAL_MASK ((1ULL << HLC_LOGICAL_BITS) - 1)
static uint64_t g_xclock = 1;
/*
 * Read horizon.  g_xclock is the newest timestamp HANDED OUT; a commit
 * holding one may still be writing its log record or applying its
 * versions.  Snapshots are taken from g_xvisible instead: every commit
 * with ts <= g_xvisible is durable and fully applied.  It advances (under
 * the commit lock) when the oldest in-flight commit finishes -- see
 * xs_inflight_remove -- so a snapshot never sees part of a transaction,
 * and a transaction never sees a row appear inside its own snapshot.
 */
static uint64_t g_xvisible = 1;
static uint64_t xs_visible(void);
static void xs_vis_wake(void);

/*
 * Wall-clock milliseconds since the epoch, the HLC physical component.
 *
 * Derived from the MONOTONIC clock plus a one-shot epoch offset rather
 * than from xtc_clock_real() directly, for two reasons:
 *
 *   1. Determinism.  xtc_clock_mono is virtualized by the deterministic
 *      simulator (xtc_clock_real is not), so a seeded DST run replays
 *      this engine's commit timestamps exactly.  Reading the wall clock
 *      here made the whole native-engine composition suite
 *      nondeterministic and tripped libxtc's determinism guard.
 *   2. Monotonicity.  An HLC's physical component must never go
 *      backward; the wall clock can (NTP step, operator change) while
 *      the monotonic clock cannot.
 *
 * The epoch offset is sampled ONCE, so stamps remain meaningful as
 * absolute wall-clock milliseconds while still advancing monotonically.
 *
 * Inside a deterministic run (the simulator's virtual clock active) the
 * physical component is the virtual milliseconds alone -- a pure
 * function of the run.  The offset is NOT added there: it was sampled
 * from the real clock before the run, so adding it carried real time
 * into every stamp of the run, and two runs of one seed minted
 * different stamps (2.0; the version keys hold the stamp, so a replay
 * then built a different tree).  The determinism guard cannot see that
 * class: the real clock was read outside the run.
 */
static uint64_t
hlc_phys_ms(void)
{
	/* mono_base/epoch_base are set together on first call.  A benign
	 * race between two first-callers only re-samples the same pair; the
	 * HLC CAS below is what actually orders stamps. */
	static int64_t epoch_off_ms; /* wall_ms - mono_ms, once */
	static int epoch_set;
	int64_t mono_ns = xtc_clock_mono();
	int64_t mono_ms = (mono_ns < 0 ? 0 : mono_ns) / 1000000LL;

	if (xtc_sim_clock_active())
		return (uint64_t)mono_ms;    /* virtual: no real-time offset */
	if (!xtc_atomic_load_i32(&epoch_set, xtc_mo_acquire)) {
		/* One wall-clock read, outside any hot/simulated path, to
		 * anchor the monotonic timeline to a human-meaningful epoch. */
		int64_t wall_ns = xtc_clock_real();
		int64_t wall_ms = (wall_ns < 0 ? 0 : wall_ns) / 1000000LL;
		xtc_atomic_store_i64(&epoch_off_ms, wall_ms - mono_ms,
		    xtc_mo_relaxed);
		xtc_atomic_store_i32(&epoch_set, 1, xtc_mo_release);
	}
	return (uint64_t)(mono_ms +
	    xtc_atomic_load_i64(&epoch_off_ms, xtc_mo_relaxed));
}

/*
 * Mint the next commit timestamp.  Read the physical clock; if it has
 * advanced past the stored physical component, adopt it with logical
 * 0; otherwise keep the stored physical component and bump the logical
 * counter (rolling into the next millisecond if the counter
 * saturates, so the stamp is always strictly greater than the last).
 * A CAS publishes the new stamp; on contention, retry.  The result is
 * unique and strictly monotonic across all callers.
 */
static uint64_t
hlc_tick(void)
{
	for (;;) {
		uint64_t cur = xtc_atomic_load_u64(&g_xclock, xtc_mo_relaxed);
		uint64_t cur_phys = cur >> HLC_LOGICAL_BITS;
		uint64_t cur_log = cur & HLC_LOGICAL_MASK;
		uint64_t now = hlc_phys_ms();
		uint64_t nxt;

		if (now > cur_phys) {
			nxt = now << HLC_LOGICAL_BITS;       /* logical 0 */
		} else if (cur_log < HLC_LOGICAL_MASK) {
			nxt = (cur_phys << HLC_LOGICAL_BITS) | (cur_log + 1);
		} else {
			nxt = (cur_phys + 1) << HLC_LOGICAL_BITS; /* carry */
		}
		if (xtc_atomic_cas_weak_u64(&g_xclock, &cur, nxt,
		        xtc_mo_relaxed, xtc_mo_relaxed))
			return nxt;
	}
}


/* STEAL: a unique key suffix for each spilled payload, and a logical id
 * for each transaction that spills.  Steal-txn ids live in a high range
 * so they never collide with commit timestamps (which come from
 * g_xclock); recovery sees a steal-txn never commits and undoes its
 * spilled payloads with CLRs. */
static uint64_t g_stage_seq = 1;
static uint64_t g_steal_txn = (1ULL << 62);

/* Count of compensation records (CLRs) written across recovery undo
 * passes -- nonzero proves the undo path ran for real (see
 * xstore_undo_clrs). */
static uint64_t g_undo_clrs = 0;

/* Optional process-global write-ahead log (see xstore_set_wal). */
static wal_t *g_xwal = NULL;
static void xs_commit_state_reset(void);
/* Attaching or detaching the log is engine setup/teardown: no commit can
 * legitimately be in flight, so drop any in-flight state a crashed or
 * abandoned run left behind (see xs_commit_state_reset). */
void
xstore_set_wal(struct wal *w)
{
	g_xwal = (wal_t *)w;
	xs_commit_state_reset();
}

/* Number of CLRs written during recovery undo so far (for tests/metrics). */
uint64_t
xstore_undo_clrs(void)
{
	return xtc_atomic_load_u64(&g_undo_clrs, xtc_mo_relaxed);
}

/* The MVCC commit clock, for the engine to persist at a clean shutdown
 * and restore when it trusts the base on restart. */
uint64_t
xstore_clock(void)
{
	return xtc_atomic_load_u64(&g_xclock, xtc_mo_relaxed);
}

struct xs_inflight;
static struct xs_inflight *g_inflight;   /* guarded by the commit lock */
/* The max commit timestamp registered per table (hashed; a collision
 * only makes the commit-time UNIQUE re-check run when it could have
 * been skipped).  Guarded by the commit lock. */
#define XS_TAB_TS_N 1024
static uint64_t g_tab_cts[XS_TAB_TS_N];
/*
 * Row stamps: the newest timestamp registered for any (tid, rid) hashing
 * to a slot (a collision only forces a check that could have been
 * skipped).  Written under the commit lock at registration, read under
 * it.  A commit probes each row's newest committed version BEFORE taking
 * the commit lock, at a horizon v0 read first; under the lock a row
 * needs the full check (probe again + in-flight scan) only if its stamp
 * > v0 -- every commit not visible at v0 has ts > v0 and stamped its rows
 * at registration, so stamp <= v0 means no in-flight writer of the row
 * and no commit of it since the early probe.  At 64 clients these probes
 * (a B-tree descent per row, latching hot leaves) ran under the lock.
 */
#define XS_ROW_TS_N 65536
static uint64_t g_row_cts[XS_ROW_TS_N];
/* The same for UNIQUE key tuples (hash of index id + key tuple): the
 * newest timestamp registered by a commit carrying the key.  Stamped
 * under the commit lock by xs_uniq_stamp_locked.  A key whose stamp is
 * <= the horizon the transaction's statement-time check ran at
 * (uniq_vis) skips the commit-time committed probe and the O(in-flight)
 * scan: no commit carrying it was in flight or has committed since. */
static uint64_t g_uk_cts[XS_ROW_TS_N];

static uint64_t xs_bytes_hash(const uint8_t *p, int n);

static unsigned
xs_uk_slot(uint32_t idx_id, const uint8_t *key, int klen)
{
	uint64_t x = xs_bytes_hash(key, klen) ^ ((uint64_t)idx_id * 0x9e3779b97f4a7c15ull);
	x ^= x >> 29;
	return (unsigned)(x & (XS_ROW_TS_N - 1));
}

static unsigned
xs_row_slot(uint32_t tid, int64_t rid)
{
	uint64_t x = ((uint64_t)tid << 40) ^ (uint64_t)rid;
	x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
	return (unsigned)(x & (XS_ROW_TS_N - 1));
}
static void xs_visible_catchup_locked(void);
/* Checkpoint gate (xstore_quiesce_begin): while set, no commit takes a
 * timestamp and no STEAL spill writes the log.  Guarded by the commit
 * lock.  g_wal_busy counts log writes in progress (any xs_wal_emit). */
static int g_ckpt_gate;
static int g_wal_busy;
/* Set by xstore_quiesce_final (the engine is stopping and holds the gate
 * for good), cleared when a log is attached (xs_commit_state_reset). */
static int g_xs_stopped;
static void xs_gate_wait_locked(void);

/* The static engine amutexes, cached: xtc_amutex_static takes a global
 * mutex on every call, so resolving it per lock and unlock put a second
 * process-wide mutex on every commit (seen in the 64-client stacks). */
static xtc_amutex_t *g_lk[XTC_AMUTEX_STATIC_MAX];

static xtc_amutex_t *
xs_lk_get(unsigned slot)
{
	xtc_amutex_t *m = xtc_atomic_load_ptr(&g_lk[slot], xtc_mo_acquire);
	if (m == NULL) {
		m = xtc_amutex_static(slot);     /* idempotent: a race is benign */
		xtc_atomic_store_ptr(&g_lk[slot], m, xtc_mo_release);
	}
	return m;
}

/* Take an engine lock (masked: see above).  Holding the commit lock
 * starts by unlinking the in-flight entries marked done meanwhile. */
static int  xs_inflight_prune_locked(int force);
static int  g_commit_depth;          /* commit-lock nesting (holder only) */
static int  g_vis_adv;               /* horizon advanced; wake at release */
static int g_ndone;                  /* in-flight entries marked done */

static void
xs_lk(unsigned slot)
{
	(void)xtc_mask_enter();
	(void)xtc_amutex_lock(xs_lk_get(slot), -1);
	if (slot == XS_LK_COMMIT && g_commit_depth++ == 0)
		(void)xs_inflight_prune_locked(0);  /* start from a clean registry */
}

static void
xs_unlk(unsigned slot)
{
	(void)xtc_amutex_unlock(xs_lk_get(slot));
	(void)xtc_mask_leave();
}

/*
 * Release the commit lock.  The outermost release first unlinks the
 * in-flight entries marked done meanwhile (advancing the horizon), and
 * after it re-checks g_ndone: a remover that marked an entry done while
 * we held the lock found it busy and left the prune to us (see
 * xs_inflight_remove), so take the lock back and prune if one is
 * pending.  Waiters the horizon now covers are woken outside the lock.
 */
static void
xs_commit_unlock(void)
{
	xtc_amutex_t *m = xs_lk_get(XS_LK_COMMIT);
	int adv = 0;
	if (--g_commit_depth > 0) {
		(void)xtc_amutex_unlock(m);   /* nested: still held */
		(void)xtc_mask_leave();
		return;
	}
	for (;;) {
		(void)xs_inflight_prune_locked(0);
		adv |= g_vis_adv;        /* any advance under this hold */
		g_vis_adv = 0;
		g_commit_depth = 0;
		(void)xtc_amutex_unlock(m);
		if (xtc_atomic_load_i32(&g_ndone, xtc_mo_seq_cst) <= 0 ||
		    xtc_amutex_try_lock(m) != XTC_OK)
			break;           /* none pending, or the new holder prunes */
		g_commit_depth = 1;
	}
	if (adv)
		xs_vis_wake();
	(void)xtc_mask_leave();
}

void
xstore_set_clock(uint64_t v)
{
	uint64_t cur;

	/* Restore-on-restart and the HLC merge-on-observe rule are the same
	 * operation: never let the clock run backwards.  Advance to v only
	 * if it is ahead of the current stamp.  When the engine is
	 * distributed, a node calls this with a timestamp received from a
	 * peer so its own stamps stay causally after everything it has
	 * observed.  Under the commit lock (timestamps are minted there), and
	 * the read horizon follows only when no commit is in flight. */
	xs_commit_lock();
	cur = xtc_atomic_load_u64(&g_xclock, xtc_mo_relaxed);
	if (v > cur)
		xtc_atomic_store_u64(&g_xclock, v, xtc_mo_relaxed);
	xs_visible_catchup_locked();
	xs_commit_unlock();
}

/* HLC merge-on-observe: fold a timestamp seen from a peer into the
 * local clock so the next minted stamp is strictly greater than it.
 * A thin alias over xstore_set_clock, named for the distributed call
 * site that will use it. */
void xstore_clock_observe(uint64_t peer_ts) { xstore_set_clock(peer_ts); }

void
xstore_clock_reset(void)
{
	xs_commit_lock();
	xtc_atomic_store_u64(&g_xclock, 1, xtc_mo_relaxed);
	xtc_atomic_store_u64(&g_xvisible, 1, xtc_mo_seq_cst);
	/* The stamps are commit times: left over from the earlier run, the
	 * re-check skips below would compare them with the restarted clock. */
	memset(g_tab_cts, 0, sizeof g_tab_cts);
	memset(g_row_cts, 0, sizeof g_row_cts);
	memset(g_uk_cts, 0, sizeof g_uk_cts);
	xs_commit_unlock();
}

#define XS_F_DELETED 0x01u     /* value[0] flag: this version is a tombstone */

/*
 * Isolation levels, mapped onto the MVCC snapshot model.  All reads
 * are against committed versions (writes buffer in wbuf until commit),
 * so there is no truly "dirty" read; the levels differ in WHEN the
 * read snapshot is taken and whether the read set is validated:
 *
 *   READ UNCOMMITTED  treated as READ COMMITTED -- an MVCC engine has
 *                     no uncommitted versions to expose.
 *   READ COMMITTED    a fresh latest-committed snapshot per statement.
 *   REPEATABLE READ   one snapshot per transaction (== SNAPSHOT here).
 *   SNAPSHOT          one snapshot per transaction (the default).
 *   SERIALIZABLE      per-transaction snapshot + Cahill SSI validation.
 *
 * The names cover both the SQL-standard set (PostgreSQL) and the
 * snapshot level; SQLite's default maps to SERIALIZABLE.
 */
#define XS_ISO_READ_UNCOMMITTED 0
#define XS_ISO_READ_COMMITTED   1
#define XS_ISO_REPEATABLE_READ  2
#define XS_ISO_SNAPSHOT         3
#define XS_ISO_SERIALIZABLE     4

/* ctx->last_conflict: a write-write conflict of the last write.  STALE:
 * the row was committed past the snapshot the write was computed
 * against (a READ COMMITTED statement may restart on a new snapshot);
 * HARD: a deadlock, or a wait that could not happen or hit the cap. */
#define XS_CONFL_STALE 1
#define XS_CONFL_HARD  2
#define XS_VKLEN     20        /* (tableid:4) + (rowid:8) + (inverted commit_ts:8) */
#define XS_VMAX      4096       /* max row payload bytes (one page-ish) */
#define XS_STAGE_TID 0xFFFFFFFEu  /* reserved table-id for STEAL spill payloads */
#define XS_SPILL_HI  (256 * 1024) /* spill buffered payloads once they exceed this */
#define XS_SPILL_LO  (64 * 1024)  /* spill down to this (hysteresis) */

/* A buffered write within an open transaction (see xs_begin/xs_commit). */
typedef struct xs_wrec {
	uint32_t tableid;
	int64_t  rowid;
	int      deleted;
	uint16_t len;
	uint8_t *data;        /* malloc'd; NULL for a delete or when spilled */
	int      spilled;     /* 1: payload is in the STEAL staging area, not data */
	int64_t  stage_id;    /* staging key suffix once spilled (0 = never);
	                       * the key stays until the entry is dropped,
	                       * even after the payload is brought back */
	uint64_t base;        /* snapshot the write was computed against: a
	                       * committed version newer than this at commit
	                       * time is a write-write conflict */
} xs_wrec_t;

/* A logical row identity: a transaction spans tables, so the read set
 * and write set key on (tableid, rowid), not rowid alone. */
typedef struct xs_rkey {
	uint32_t tableid;
	int64_t  rowid;
} xs_rkey_t;

/* Per-connection state, shared by the module and the SQL functions:
 * the engine storage, this connection's read snapshot, and the
 * in-flight transaction's buffered writes. */
typedef struct ssi_txn ssi_txn_t;     /* SSI registry slot (defined below) */
typedef struct xstore_ctx {
	bt_t            *bt;
	uint64_t read_snap;           /* 0 == read latest committed */
	int              in_txn;      /* between xBegin and xCommit/xRollback */
	uint64_t         txn_snap;    /* snapshot captured at xBegin */
	xs_wrec_t       *wbuf;        /* buffered writes, applied atomically */
	int              wn, wcap;
	int              serializable; /* 1 == validate the read set on commit */
	int              isolation;   /* session level, XS_ISO_*; default SNAPSHOT */
	int              txn_iso;     /* level of the open txn (SET TRANSACTION) */
	int              touched;     /* open txn has read or written */
	uint64_t         stmt_base;   /* snapshot the current DML statement reads */
	uint64_t         last_ts;     /* commit ts of this connection's last write */
	int              last_conflict; /* last write's conflict: XS_CONFL_* */
	int              stmt_puts;   /* autocommit rows applied by this statement */
	int              stmt_first;  /* this statement is the txn's first access */
	xs_rkey_t       *rset;        /* (table,rowid) read in this txn (serializable) */
	int              rn, rcap;
	int              did_scan;    /* txn did a full scan (table-level read) */
	ssi_txn_t       *ssi;         /* this txn's SSI registry slot (or NULL) */
	int              snap_slot;   /* GC snapshot-hold slot (-1 == none) */
	int              autovacuum;  /* 1 == prune dead versions inline on write */
	uint64_t         steal_txn;   /* STEAL: logical id once this txn spills (0 = none) */
	size_t           wbuf_bytes;  /* buffered payload bytes (spill trigger) */

	/*
	 * Savepoint stack for nested transactions.  SQLite drives
	 * xSavepoint/xRelease/xRollbackTo with a savepoint index; we record
	 * for each active savepoint level the write-buffer and read-set
	 * positions at the moment it opened.  Because writes are appended
	 * to wbuf (newest-wins on read-your-writes) and reads to rset,
	 * rolling back to a savepoint is just truncating both back to the
	 * recorded marks -- the nested transaction's effects vanish and the
	 * parent sees exactly what it saw before, the Berkeley DB / nested
	 * top-action child-abort model mapped onto buffered writes.
	 * Releasing a savepoint merges the child into the parent: nothing
	 * to undo, the writes simply remain in wbuf.
	 */
	int             *sp_wn;       /* wbuf position at each open savepoint */
	int             *sp_rn;       /* rset position at each open savepoint */
	int              sp_n, sp_cap;

	/*
	 * Native-driver transaction ownership.  When native_mode is set (by
	 * the native sx_stmt driver, which never calls the SQLite VDBE),
	 * xs_enter_ctx consults native_autocommit BELOW instead of SQLite's
	 * xsql_get_autocommit(db) -- the native driver, not SQLite, owns
	 * autocommit / BEGIN state.  native_autocommit == 1 means autocommit
	 * (no open explicit txn); the driver clears it on BEGIN and sets it
	 * on COMMIT/ROLLBACK.  When native_mode is 0 the behavior is exactly
	 * the SQLite-driven path (default), so this is inert until a native
	 * driver opts in.
	 */
	int              native_mode;       /* 1 == driver owns autocommit */
	int              native_autocommit; /* 1 == autocommit (native_mode only) */
	int              implicit;     /* an autocommit statement's own txn */
	uint64_t         stmt_snap;    /* the statement's pinned snapshot */

	/* (tableid,rowid) -> newest wbuf index (O(1) read-your-writes) and
	 * (unique index, key tuple hash) -> wbuf index (own-write UNIQUE
	 * check).  See wmap_put / umap_put. */
	struct xs_wmap { uint32_t tid; int64_t rid; int wi; } *wmap;
	int              wmap_n, wmap_cap, wmap_bad;
	struct xs_umap { uint64_t h; uint32_t idx_id; int wi; } *umap;
	int              umap_n, umap_cap;
	int              umap_bad;    /* a row is missing from the umap */
	uint64_t         uniq_vis;    /* horizon of the txn's UNIQUE checks */
	uint64_t         uniq_gen;    /* index generation at those checks */
	char             uniq_err[192]; /* last UNIQUE violation message */
	char             rd_err[192];   /* this statement's first read error */
	uint32_t         rd_tid;        /* or a table read error, named later */
	int              rd_rc;         /* its XTC_E_* code; 0: none */
	/* Largest buffered rowid per table (xstore_max_rowid_txn), kept by
	 * wbuf_add; a truncate/clear invalidates it (wmax_ok = 0) and the
	 * next query rebuilds it with one pass. */
	struct { uint32_t tableid; int64_t max; } wmax[8];
	int              wmax_n, wmax_ok;
	/* Row write intents held by this connection's transaction (see
	 * "row write intents" below): slot in g_ilk_owner, txn sequence, and
	 * the held keys (released by wbuf_clear). */
	int              ilk_slot;
	uint64_t         ilk_seq;
	struct xs_ikey { uint32_t tid; int64_t rid; } *ilk;
	int              ilk_n, ilk_cap;
	int              ilk_skip;      /* this write is a fresh INSERT */
} xstore_ctx_t;

static void xs_intents_release(xstore_ctx_t *c);
static void xs_ilk_reset(void);

/* Note `rowid` of `tableid` in the per-table buffered max; 0 when the
 * table has no slot (more than 8 tables written: callers scan). */
static int
wmax_note(xstore_ctx_t *c, uint32_t tableid, int64_t rowid)
{
	int i;
	for (i = 0; i < c->wmax_n; i++)
		if (c->wmax[i].tableid == tableid) {
			if (rowid > c->wmax[i].max) c->wmax[i].max = rowid;
			return 1;
		}
	if (c->wmax_n == (int)(sizeof c->wmax / sizeof c->wmax[0])) return 0;
	c->wmax[c->wmax_n].tableid = tableid;
	c->wmax[c->wmax_n++].max = rowid;
	return 1;
}

static void wmap_put(xstore_ctx_t *c, uint32_t tid, int64_t rid, int wi);
static int  wmap_get(const xstore_ctx_t *c, uint32_t tid, int64_t rid);
static void wmaps_rebuild(xstore_ctx_t *c);
static void xs_umap_index(xstore_ctx_t *c, int wi);

/* Free and truncate wbuf entries [mark, wn), restoring the byte count.
 * Used by savepoint rollback (and reused by wbuf_clear via mark 0). */
/*
 * Delete an entry's STEAL staging row, if it ever spilled.  Nothing did:
 * every spilled payload stayed in the tree after its transaction ended,
 * so a bulk load left a second copy of its rows in the page file (until a
 * crash rebuild, whose loser undo removes them).  No log record is
 * needed: the spill's log record belongs to a steal transaction that
 * never commits, so recovery redoes and then undoes it anyway -- the
 * same end state.  The key is private to this transaction.
 */
static void enc_vkey(uint32_t tableid, int64_t rowid, uint64_t commit_ts,
    uint8_t out[XS_VKLEN]);
static void
xs_stage_drop(xstore_ctx_t *c, xs_wrec_t *w)
{
	uint8_t key[XS_VKLEN];
	if (w->stage_id == 0)
		return;
	enc_vkey(XS_STAGE_TID, w->stage_id, 0, key);
	(void)bt_delete(c->bt, key, XS_VKLEN);
	w->stage_id = 0;
	w->spilled = 0;
}

static void
wbuf_truncate(xstore_ctx_t *c, int mark)
{
	int i;
	if (mark < 0)
		mark = 0;
	for (i = mark; i < c->wn; i++) {
		xs_stage_drop(c, &c->wbuf[i]);
		if (c->wbuf[i].data != NULL) {
			if (c->wbuf[i].len <= c->wbuf_bytes)
				c->wbuf_bytes -= c->wbuf[i].len;
			free(c->wbuf[i].data);
			c->wbuf[i].data = NULL;
		}
	}
	c->wn = mark;
	c->wmax_ok = 0;
	wmaps_rebuild(c);
}

static void
wbuf_clear(xstore_ctx_t *c)
{
	int i;
	for (i = 0; i < c->wn; i++) {
		xs_stage_drop(c, &c->wbuf[i]);
		free(c->wbuf[i].data);
	}
	c->wn = 0;
	c->wbuf_bytes = 0;
	c->sp_n = 0;          /* all savepoints close at end of transaction */
	for (i = 0; i < c->wmap_cap; i++) c->wmap[i].wi = -1;
	for (i = 0; i < c->umap_cap; i++) c->umap[i].wi = -1;
	c->wmap_n = c->umap_n = 0;
	c->wmap_bad = 0;
	c->umap_bad = 0;
	c->uniq_vis = 0;
	c->wmax_n = 0;
	c->wmax_ok = 1;
	xs_intents_release(c);   /* the txn's versions are in (or discarded) */
}
static int
wbuf_add(xstore_ctx_t *c, uint32_t tableid, int64_t rowid, const void *blob, int n, int deleted)
{
	xs_wrec_t *w;
	if (c->wn == c->wcap) {
		int nc = c->wcap ? c->wcap * 2 : 16;
		xs_wrec_t *nb = realloc(c->wbuf, (size_t)nc * sizeof *nb);
		if (nb == NULL) return SX_NOMEM;
		c->wbuf = nb; c->wcap = nc;
	}
	w = &c->wbuf[c->wn];
	w->tableid = tableid; w->rowid = rowid; w->deleted = deleted;
	w->len = 0; w->data = NULL; w->spilled = 0; w->stage_id = 0;
	/* Snapshot levels compute every write against txn_snap; READ
	 * COMMITTED against the statement's latest-committed view. */
	w->base = c->txn_iso >= XS_ISO_REPEATABLE_READ ? c->txn_snap
	    : c->stmt_base;
	if (!deleted && n > 0) {
		if (n > XS_VMAX) n = XS_VMAX;
		w->data = malloc((size_t)n);
		if (w->data == NULL) return SX_NOMEM;
		memcpy(w->data, blob, (size_t)n);
		w->len = (uint16_t)n;
		c->wbuf_bytes += (size_t)n;
	}
	c->wn++;
	if (c->wmax_ok && !wmax_note(c, tableid, rowid)) c->wmax_ok = 0;
	wmap_put(c, tableid, rowid, c->wn - 1);
	xs_umap_index(c, c->wn - 1);
	return SX_OK;
}
/* Read-your-writes: the newest buffered write for `rowid`, or NULL. */
static const xs_wrec_t *
wbuf_find(const xstore_ctx_t *c, uint32_t tableid, int64_t rowid)
{
	int i;
	if (!c->wmap_bad) {
		i = wmap_get(c, tableid, rowid);
		return i >= 0 ? &c->wbuf[i] : NULL;
	}
	for (i = c->wn - 1; i >= 0; i--)
		if (c->wbuf[i].rowid == rowid && c->wbuf[i].tableid == tableid)
			return &c->wbuf[i];
	return NULL;
}

/* Is wbuf[wi] the newest buffered write of its row? */
static int
wbuf_is_newest(const xstore_ctx_t *c, int wi)
{
	return wbuf_find(c, c->wbuf[wi].tableid, c->wbuf[wi].rowid) == &c->wbuf[wi];
}

/* Record a (table,rowid) in the transaction's read set (serializable). */
static void
rset_add(xstore_ctx_t *c, uint32_t tableid, int64_t rowid)
{
	int i;
	for (i = 0; i < c->rn; i++)
		if (c->rset[i].rowid == rowid && c->rset[i].tableid == tableid)
			return;   /* already recorded */
	if (c->rn == c->rcap) {
		int nc = c->rcap ? c->rcap * 2 : 16;
		xs_rkey_t *nb = realloc(c->rset, (size_t)nc * sizeof *nb);
		if (nb == NULL) return;            /* best-effort; a miss is safe-ish */
		c->rset = nb; c->rcap = nc;
	}
	c->rset[c->rn].tableid = tableid;
	c->rset[c->rn].rowid = rowid;
	c->rn++;
}

/*
 * Serializable Snapshot Isolation (Cahill et al., SIGMOD 2008).  The
 * read-set check in
 * xs_sync alone is conservative "precision validation" -- it aborts a
 * transaction whenever something it read was overwritten, i.e. on any
 * outgoing rw-antidependency.  SSI is more permissive: it aborts only
 * a PIVOT -- a transaction with BOTH an incoming and an outgoing
 * rw-antidependency -- because Fekete et al. (2005) proved every
 * serialization-graph cycle under snapshot isolation contains such a
 * pivot, so aborting all pivots breaks all cycles while letting
 * read-mostly transactions (outgoing edge only) commit.
 *
 * The outgoing edge (something I read was overwritten by a committed
 * transaction after my snapshot) is detected from the shared B-tree's
 * version timestamps (xs_newer_than), so it catches overwrites by any
 * committer regardless of its isolation level.  The incoming edge
 * (someone read what I am about to write) cannot be read from the
 * B-tree -- reads leave no version -- so serializable transactions
 * publish their read sets to this small in-memory registry, and a
 * committing writer scans it for a concurrent reader of any rowid it
 * writes.  The outgoing edge counts toward an
 * abort only when its target has already COMMITTED (the pivot's
 * out-neighbor commits first): so in a write-skew the first committer
 * (whose out-neighbor is still active) commits and the second aborts.
 */
#define SSI_MAX 256
enum { SSI_FREE = 0, SSI_ACTIVE, SSI_COMMITTED };
struct ssi_txn {
	int       state;
	uint64_t  snap;        /* read snapshot */
	uint64_t  commit_ts;   /* set when COMMITTED */
	xs_rkey_t *reads;      /* (table,rowid) this txn read (point predicates) */
	int       nreads, rcap;
	struct ssi_rng { uint32_t tableid; int64_t lo, hi; } *rngs;  /* range predicates */
	int       nrng, rngcap;
};

static ssi_txn_t       g_ssi[SSI_MAX];

/* Free committed slots no active transaction can still conflict with:
 * a committed W is needed only while some active U started before W
 * committed (U.snap < W.commit_ts).  Caller holds the SSI lock. */
static void
ssi_gc_locked(void)
{
	uint64_t min_active = ~(uint64_t)0;
	int i;
	for (i = 0; i < SSI_MAX; i++)
		if (g_ssi[i].state == SSI_ACTIVE && g_ssi[i].snap < min_active)
			min_active = g_ssi[i].snap;
	for (i = 0; i < SSI_MAX; i++)
		if (g_ssi[i].state == SSI_COMMITTED &&
		    g_ssi[i].commit_ts <= min_active) {
			free(g_ssi[i].reads);
			g_ssi[i].reads = NULL;
			g_ssi[i].nreads = g_ssi[i].rcap = 0;
			free(g_ssi[i].rngs);
			g_ssi[i].rngs = NULL;
			g_ssi[i].nrng = g_ssi[i].rngcap = 0;
			g_ssi[i].state = SSI_FREE;
		}
}

/* Begin tracking a serializable transaction.  Returns its slot, or
 * NULL if the registry is full (the caller then falls back to
 * precision validation, which is correct, just less permissive). */
static ssi_txn_t *
ssi_begin(uint64_t snap)
{
	ssi_txn_t *t = NULL;
	int i;
	xs_ssi_lock();
	ssi_gc_locked();
	for (i = 0; i < SSI_MAX; i++)
		if (g_ssi[i].state == SSI_FREE) {
			t = &g_ssi[i];
			t->state = SSI_ACTIVE;
			t->snap = snap;
			t->commit_ts = 0;
			t->nreads = 0;        /* keep any allocated rcap/reads */
			t->nrng = 0;          /* keep any allocated rngcap/rngs */
			break;
		}
	xs_ssi_unlock();
	return t;
}

static void
ssi_record_read(ssi_txn_t *t, uint32_t tableid, int64_t rowid)
{
	int i;
	if (t == NULL) return;
	xs_ssi_lock();
	for (i = 0; i < t->nreads; i++)
		if (t->reads[i].rowid == rowid && t->reads[i].tableid == tableid) {
			xs_ssi_unlock(); return;
		}
	if (t->nreads == t->rcap) {
		int nc = t->rcap ? t->rcap * 2 : 16;
		xs_rkey_t *nb = realloc(t->reads, (size_t)nc * sizeof *nb);
		if (nb == NULL) { xs_ssi_unlock(); return; }
		t->reads = nb; t->rcap = nc;
	}
	t->reads[t->nreads].tableid = tableid;
	t->reads[t->nreads].rowid = rowid;
	t->nreads++;
	xs_ssi_unlock();
}

/* Record a RANGE predicate [lo, hi] this txn read (a scan).  A full
 * table scan records [INT64_MIN, INT64_MAX]; a bounded scan its actual
 * key bounds.  This is the SSI predicate lock: a future
 * writer's rowid creates an incoming rw-edge only if it falls inside a
 * range some concurrent reader actually scanned -- not "any write vs
 * any scan". */
#if defined(SQLXTC_VTAB)
static void
ssi_record_range(ssi_txn_t *t, uint32_t tableid, int64_t lo, int64_t hi)
{
	if (t == NULL || lo > hi) return;
	xs_ssi_lock();
	if (t->nrng == t->rngcap) {
		int nc = t->rngcap ? t->rngcap * 2 : 8;
		struct ssi_rng *nb = realloc(t->rngs, (size_t)nc * sizeof *nb);
		if (nb == NULL) { xs_ssi_unlock(); return; }
		t->rngs = nb; t->rngcap = nc;
	}
	t->rngs[t->nrng].tableid = tableid;
	t->rngs[t->nrng].lo = lo;
	t->rngs[t->nrng].hi = hi;
	t->nrng++;
	xs_ssi_unlock();
}
#endif /* SQLXTC_VTAB */

/* Does any OTHER serializable transaction, concurrent with `me`, have
 * an incoming rw-edge to me -- i.e. did it read a rowid in my write
 * set `wr[0..nwr)` (or scan the whole table)?  Returns 1 if so, or if
 * `me` is NULL (registry full -> conservative). */
static int
ssi_in_conflict(ssi_txn_t *me, const xs_rkey_t *wr, int nwr)
{
	int i, j, k, in = 0;
	if (me == NULL) return 1;
	if (nwr == 0) return 0;
	xs_ssi_lock();
	for (i = 0; i < SSI_MAX && !in; i++) {
		ssi_txn_t *u = &g_ssi[i];
		if (u == me || u->state == SSI_FREE) continue;
		/* Concurrent: an active txn always overlaps me; a committed
		 * one only if it committed after my snapshot (I didn't see
		 * it).  My own commit is in the future, so it never saw me. */
		if (u->state == SSI_COMMITTED && u->commit_ts <= me->snap) continue;
		for (k = 0; k < nwr; k++) {
			for (j = 0; j < u->nreads; j++)
				if (u->reads[j].rowid == wr[k].rowid &&
				    u->reads[j].tableid == wr[k].tableid) { in = 1; break; }
			if (in) break;
			for (j = 0; j < u->nrng; j++)
				if (u->rngs[j].tableid == wr[k].tableid &&
				    wr[k].rowid >= u->rngs[j].lo &&
				    wr[k].rowid <= u->rngs[j].hi) {
					in = 1; break;
				}
			if (in) break;
		}
	}
	xs_ssi_unlock();
	return in;
}

static void
ssi_commit(ssi_txn_t *t, uint64_t commit_ts)
{
	if (t == NULL) return;
	xs_ssi_lock();
	t->state = SSI_COMMITTED;
	t->commit_ts = commit_ts;
	xs_ssi_unlock();
}

static void
ssi_abort(ssi_txn_t *t)
{
	if (t == NULL) return;
	xs_ssi_lock();
	t->state = SSI_FREE;
	t->nreads = 0;
	t->nrng = 0;
	xs_ssi_unlock();
}

/*
 * Snapshot-hold registry, for the version-GC horizon.  A version is
 * dead once NO live read snapshot can reach it: per rowid, every
 * version older than the newest one at-or-before the oldest live
 * snapshot is unreachable (a tombstone that is itself the newest such
 * version, with nothing newer, is unreachable too).  This is the
 * MVCC vacuum horizon -- the oldest snapshot any live transaction can see.
 *
 * Every snapshot a reader is using pins the horizon: an open explicit
 * transaction (txn_snap, held to commit), an xstore_as_of() time-travel
 * pin (read_snap), and an autocommit statement's snapshot (stmt_base,
 * for the statement, xstore_stmt_begin/_end).  An unpinned reader is NOT
 * safe: a reader at p needs the newest version at or below p, and once a
 * newer version commits and the horizon passes it, that version is
 * garbage -- an autocommit UPDATE then found no row (a lost update) and
 * a SELECT skipped it (test_snap_registry (3)).  An as_of pin set below
 * the reclaimed horizon is best-effort (its versions may already be
 * gone); explicit transactions always snapshot the current clock and
 * are never below the horizon.
 */
#define SNAP_MAX 256
/*
 * The registry is lock-free on the transaction path.  It used one global
 * mutex for every pin and release -- taken at every BEGIN and COMMIT,
 * even a read-only transaction's -- and the horizon scan held it across
 * all 256 slots: at 8 threads a point-read transaction mix spent ~70% of
 * its time in futex wait/wake on it (233k txn/s at 1 thread, 105k at 8).
 * Now a connection CLAIMS a slot once (a CAS from free to "claimed, no
 * pin"), keeps it for its lifetime, and pins or releases with one
 * atomic store; the horizon is a lock-free scan of the slots.  This is
 * the per-transaction read view of InnoDB's trx_sys / PostgreSQL's
 * per-backend xmin: each owner publishes its own, the vacuum reads them
 * all.
 *
 * Race: a reader reads visible = p, a scanner reads now > p and passes
 * the reader's slot, then the reader stores p -- the horizon would pass a
 * live snapshot.  (The mutex version had the same window: the scanner
 * read `now` before taking the lock.)  snap_pin_visible closes it the
 * hazard-pointer way: store the pin, then re-read visible; if it moved,
 * pin the new value and check again.  In the seq_cst order a scanner
 * that missed the store read its slot before it, so its `now` load came
 * before the re-read, which returned p: now <= p.
 */
#define SNAP_FREE    0u
#define SNAP_CLAIMED 1u    /* owned, no pin (pins are commit timestamps > 1) */
static uint64_t g_holds[SNAP_MAX];
static uint64_t g_gc_horizon = 0; /* newest reclaimed commit_ts */

/* Set connection slot *slotp to pin `ts` (0 releases the pin; the slot
 * stays claimed by the connection until snap_slot_free). */
static void
snap_set(int *slotp, uint64_t ts)
{
	int i;
	if (*slotp < 0) {
		if (ts == 0)
			return;                       /* nothing to release */
		for (i = 0; i < SNAP_MAX; i++) {
			uint64_t want = SNAP_FREE;
			if (xtc_atomic_cas_u64(&g_holds[i], &want, SNAP_CLAIMED,
			        xtc_mo_acq_rel, xtc_mo_relaxed)) {
				*slotp = i;
				break;
			}
		}
		/* registry full: leave unpinned -- horizon stays conservative
		 * (g_xclock-bounded), so correctness holds; only an as_of pin
		 * could then be GC'd early. */
		if (*slotp < 0)
			return;
	}
	xtc_atomic_store_u64(&g_holds[*slotp], ts != 0 ? ts : SNAP_CLAIMED,
	    xtc_mo_seq_cst);
}

/* Pin and return a snapshot of the current visible horizon (see the
 * race note above).  The slot must be claimable; if the registry is
 * full the snapshot is simply unpinned (as snap_set). */
static uint64_t
snap_pin_visible(int *slotp)
{
	uint64_t p = xs_visible(), q;
	for (;;) {
		snap_set(slotp, p);
		q = xs_visible();
		if (q == p || *slotp < 0)
			return p;
		p = q;
	}
}

/* Give the connection's slot back (connection close). */
static void
snap_slot_free(int *slotp)
{
	if (*slotp >= 0) {
		xtc_atomic_store_u64(&g_holds[*slotp], SNAP_FREE,
		    xtc_mo_release);
		*slotp = -1;
	}
}

/* The GC horizon: the oldest live pinned snapshot, or `now` if none.
 * `now` must have been read (xs_visible) before this call. */
/* test_prune_horizon counts the computations (it #includes xstore.c). */
#ifndef XS_HORIZON_TEST_HOOK
#define XS_HORIZON_TEST_HOOK() ((void)0)
#endif
static uint64_t
snap_horizon(uint64_t now)
{
	uint64_t h = now;
	int i;
	XS_HORIZON_TEST_HOOK();
	xtc_atomic_thread_fence(xtc_mo_seq_cst);
	for (i = 0; i < SNAP_MAX; i++) {
		uint64_t v = xtc_atomic_load_u64(&g_holds[i], xtc_mo_seq_cst);
		if (v > SNAP_CLAIMED && v < h) h = v;
	}
	return h;
}

/* Republish this connection's effective GC pin: its open-transaction
 * snapshot, else its as_of read snapshot, else none.  Called wherever
 * those change. */
static void
xs_pin_recompute(xstore_ctx_t *cx)
{
	uint64_t pin = 0;
	if (cx->in_txn)
		pin = cx->txn_snap;
	else {
		uint64_t rs =
		    xtc_atomic_load_u64(&cx->read_snap, xtc_mo_relaxed);
		if (rs != 0) pin = rs;
	}
	snap_set(&cx->snap_slot, pin);
}

/* Order-preserving big-endian; the timestamp half is inverted so that,
 * within a rowid, higher commit_ts sorts first (newest version first). */
static void
enc_vkey(uint32_t tableid, int64_t rowid, uint64_t commit_ts,
    uint8_t out[XS_VKLEN])
{
	uint64_t r = (uint64_t)rowid ^ 0x8000000000000000ull;
	uint64_t t = ~commit_ts;
	int i;
	/* tableid first (big-endian) so versions group + sort by table. */
	for (i = 3; i >= 0; i--) { out[i] = (uint8_t)(tableid & 0xFF); tableid >>= 8; }
	for (i = 7; i >= 0; i--) { out[4 + i] = (uint8_t)(r & 0xFF); r >>= 8; }
	for (i = 7; i >= 0; i--) { out[12 + i] = (uint8_t)(t & 0xFF); t >>= 8; }
}

static uint32_t
dec_tableid(const uint8_t *k)
{
	uint32_t v = 0;
	int i;
	for (i = 0; i < 4; i++) v = (v << 8) | k[i];
	return v;
}
static int64_t
dec_rowid(const uint8_t *k)
{
	uint64_t r = 0;
	int i;
	for (i = 0; i < 8; i++) r = (r << 8) | k[4 + i];
	return (int64_t)(r ^ 0x8000000000000000ull);
}
static uint64_t
dec_ts(const uint8_t *k)
{
	uint64_t t = 0;
	int i;
	for (i = 0; i < 8; i++) t = (t << 8) | k[12 + i];
	return ~t;
}

/* STEAL: bring a spilled payload back into memory (read it from the
 * staging area into w->data) so the buffered write can be read or
 * committed.  A no-op for entries that are not spilled.  Returns
 * SX_OK or an error. */
static int
xs_wrec_ensure(xstore_ctx_t *cx, xs_wrec_t *w)
{
	uint8_t key[XS_VKLEN];
	uint16_t vl = 0;

	if (!w->spilled)
		return SX_OK;
	w->data = malloc(w->len ? w->len : 1);
	if (w->data == NULL)
		return SX_NOMEM;
	if (w->len) {
		enc_vkey(XS_STAGE_TID, w->stage_id, 0, key);
		if (bt_lookup(cx->bt, key, XS_VKLEN, w->data, w->len, &vl) != XTC_OK ||
		    vl != w->len) {
			free(w->data); w->data = NULL;
			return SX_ERROR;
		}
	}
	w->spilled = 0;
	cx->wbuf_bytes += w->len;
	return SX_OK;
}

/* ---- vtab + cursor ---- */
#if defined(SQLXTC_VTAB)
typedef struct xstore_vtab {
	xsql_vtab base;
	xstore_ctx_t *ctx;
	xsql      *db;     /* for xsql_get_autocommit: detect explicit txn */
	int        npay;   /* payload columns (declared columns minus the key) */
	uint32_t   tableid; /* version-namespace id (key prefix) for this table */
} xstore_vtab_t;
#endif

/*
 * Table-id assignment.  A table's version-namespace id (the key prefix
 * for all its versions) is allocated from a persisted on-disk catalog
 * kept at reserved table-id 0: a catalog row is keyed (0, id, commit_ts)
 * with the table's name as its value.  Ids are dense and allocated
 * sequentially (max existing id + 1), so distinct names never collide
 * -- unlike a name hash, which silently aliases two tables that hash
 * equal.
 *
 * The catalog is authoritative and recovery-safe: every CREATE writes
 * its (0, id, name) row through xs_put, so it is WAL-logged and replayed
 * into a fresh B-tree like any other version.  After recovery a
 * reconnecting vtab finds its id by name in the catalog, and the next
 * allocation continues past the recovered high-water id.  ALTER TABLE
 * RENAME writes a new version of the same id's catalog row with the new
 * name (the id -- and the table's data -- are preserved); the old name
 * then resolves to nothing and is free to reuse.
 *
 * Id 0 is reserved for the catalog itself.  An in-process cache (keyed
 * by B-tree + name) shortcuts the catalog scan for tables already seen
 * in this process, and reserves a just-allocated id so a concurrent
 * allocator on another connection picks a higher one.
 *
 * An id is never reused.  DROP TABLE tombstones the catalog row, and
 * version GC and the log dump remove a tombstone once no snapshot needs
 * it (taking the dropped table's rows with it), so the catalog alone
 * forgets a dropped id.  The catalog therefore also holds a HIGH-WATER
 * row at rowid 0 (xs_hw_*): the largest id a DROP removed, raised before
 * the tombstone is written.  Its value has an empty name (a NUL, then
 * the id in decimal), so no name lookup matches it, and a store written
 * this way stays readable by a release that does not know the row.  The
 * index catalog keeps the same row for index ids.  GC and the dump keep
 * a tombstone the mark does not cover (a DROP by an older release), in
 * either catalog.
 * One case is not covered: a store on which an older release already
 * collected a dropped index's tombstone can hand that index id out
 * again (tables have the data probe, xs_data_max_id; indexes have
 * nothing to probe).  That is harmless: the DROP cleared the entries.
 */
#define XS_CAT_TABLEID 0u
#define XS_CAT_MAX 256
static struct xs_cat_ent {
	bt_t    *bt;
	char     name[64];
	uint32_t tableid;
	char     coldefs[512];   /* cached schema, so a concurrent reader sees
	                          * it the instant CREATE caches the entry --
	                          * before the persisted catalog row is
	                          * necessarily visible to xs_cat_value. */
} g_cat[XS_CAT_MAX];
static int g_cat_n;
/* Bumped under the cat lock by every change to g_cat or g_idx, so a
 * thread-local copy of a lookup (xs_tcache_*) stays valid while it is
 * unchanged: the per-statement catalog reads then take no global lock. */
static uint64_t g_catc_gen = 1;
#define XS_CATC_BUMP() xtc_atomic_fetch_add_u64(&g_catc_gen, 1, xtc_mo_release)

/* This thread's copies of recently used g_cat entries (see g_catc_gen). */
#define XS_TCAT_N 16
static _Thread_local struct {
	uint64_t           gen;
	struct xs_cat_ent  e;
} t_cat[XS_TCAT_N];
static _Thread_local unsigned t_cat_next;

/* A g_cat entry for (bt, name) valid at the current generation: copies
 * its id / coldefs (coldefs: a 512-byte buffer, "" if none).  0 = miss. */
static int
xs_tcat_get(bt_t *bt, const char *name, uint32_t *id, char *coldefs)
{
	uint64_t g = xtc_atomic_load_u64(&g_catc_gen, xtc_mo_acquire);
	int k;
	for (k = 0; k < XS_TCAT_N; k++)
		if (t_cat[k].gen == g && t_cat[k].e.bt == bt &&
		    strcasecmp(t_cat[k].e.name, name) == 0) {
			if (id) *id = t_cat[k].e.tableid;
			if (coldefs) memcpy(coldefs, t_cat[k].e.coldefs, sizeof t_cat[k].e.coldefs);
			return 1;
		}
	return 0;
}

/* Caller holds the cat lock: remember entry `e` at this generation. */
static void
xs_tcat_put_locked(const struct xs_cat_ent *e)
{
	unsigned k = t_cat_next++ % XS_TCAT_N;
	t_cat[k].gen = xtc_atomic_load_u64(&g_catc_gen, xtc_mo_relaxed);
	t_cat[k].e = *e;
}

/* View registry: CREATE VIEW name AS <select> stored as the SELECT
 * text, keyed by (bt, name).  Views are query rewrites, not stored
 * tables; the executor expands a FROM reference to a view as a derived
 * table (its SELECT).  Protected by the catalog lock (shared with
 * cache).  In-process only for now (recreated each session, like
 * SQLite re-reads sqlite_master); persistence is a follow-up. */
#define XS_VIEW_MAX 128
static struct xs_view_ent {
	bt_t *bt;
	char  name[64];
	char  sql[1024];   /* the view's SELECT text */
} g_view[XS_VIEW_MAX];
static int g_view_n;
static int g_view_any; /* set once any view exists (sticky) */

/* Connection -> B-tree association, so a from-scratch executor driven
 * from a connection handle (xstore_bt_of) can open a storage-native
 * scan over the same B-tree the connection's xstore tables use. */
#define XS_DBMAP_MAX 1024   /* open connections (>= the server's default max_clients) */
static struct xs_dbmap_ent { struct xsql *db; bt_t *bt; xstore_ctx_t *ctx; } g_dbmap[XS_DBMAP_MAX];
static int g_dbmap_n;
/* Bumped (under the cat lock) by every map change.  The lookup below is
 * called many times per statement by every connection; a linear search
 * under the global catalog mutex made that mutex the server's
 * serializer (each contended release is a cross-loop wake).  A
 * per-thread copy of the last entry is valid while the generation is
 * unchanged: an entry only changes at register / unregister. */
static uint64_t g_dbmap_gen = 1;
/* A few entries per thread: in the server a loop thread runs several
 * connection fibers, which with a one-entry cache evicted each other on
 * every statement -- and every miss took the catalog lock (at 32
 * connections on 8 loops most of the parked connection fibers were
 * queued on it, a cross-loop hand-off per statement). */
#define XS_TDBMAP_N 16
static _Thread_local struct { uint64_t gen; struct xs_dbmap_ent e; } t_dbmap[XS_TDBMAP_N];
static _Thread_local unsigned t_dbmap_next;

static const struct xs_dbmap_ent *
xs_dbmap_get(struct xsql *db)
{
	uint64_t g = xtc_atomic_load_u64(&g_dbmap_gen, xtc_mo_acquire);
	int i, k, found = 0;
	for (k = 0; k < XS_TDBMAP_N; k++)
		if (t_dbmap[k].gen == g && t_dbmap[k].e.db == db)
			return &t_dbmap[k].e;
	k = (int)(t_dbmap_next++ % XS_TDBMAP_N);
	xs_cat_lock();
	g = xtc_atomic_load_u64(&g_dbmap_gen, xtc_mo_relaxed);
	for (i = 0; i < g_dbmap_n; i++)
		if (g_dbmap[i].db == db) { t_dbmap[k].e = g_dbmap[i]; found = 1; break; }
	xs_cat_unlock();
	if (!found) return NULL;
	t_dbmap[k].gen = g;
	return &t_dbmap[k].e;
}

bt_t *
xstore_bt_of(struct xsql *db)
{
	const struct xs_dbmap_ent *e = xs_dbmap_get(db);
	return e != NULL ? e->bt : NULL;
}

/* The connection's transaction context (the per-connection xstore_ctx_t
 * registered by xstore_register), or NULL if the connection is not
 * xstore-backed.  Lets the native write path buffer into the SAME
 * transaction buffer the vtab path uses, so native and VDBE writes in
 * one BEGIN..COMMIT commit atomically at one timestamp. */
static xstore_ctx_t *
xstore_ctx_of(struct xsql *db)
{
	const struct xs_dbmap_ent *e = xs_dbmap_get(db);
	return e != NULL ? e->ctx : NULL;
}

static int g_row_failat;    /* test hook: the row read to fail, 1-based */
static int g_row_faults;    /* test hook: reads it has failed */
static uint64_t g_rd_errors; /* reads that failed (metric) */

/* Count a read of the tree that failed with rc (any XTC_E_* but
 * XTC_E_NOTFOUND): the read-error metric (xstore_read_errors).  Returns
 * rc, so a caller can count and return in one step. */
static int
xs_rd_count(int rc)
{
	if (rc != XTC_OK && rc != XTC_E_NOTFOUND)
		xtc_atomic_fetch_add_u64(&g_rd_errors, 1, xtc_mo_relaxed);
	return rc;
}

/* Should this read of one row's or one catalog entry's versions, outside
 * a scan, fail (xstore_row_read_fail_at)?  One relaxed load when
 * disarmed. */
static int
xs_row_fault(void)
{
	if (xtc_atomic_load_i32(&g_row_failat, xtc_mo_relaxed) <= 0 ||
	    xtc_atomic_fetch_sub_i32(&g_row_failat, 1, xtc_mo_relaxed) != 1)
		return 0;
	(void)xtc_atomic_fetch_add_i32(&g_row_faults, 1, xtc_mo_relaxed);
	return 1;
}

int
xstore_row_read_fail_at(int n)
{
	return xtc_atomic_exchange_i32(&g_row_failat, n, xtc_mo_relaxed);
}

static int g_reap_fail;     /* test hook: every reap read fails */

/* Should this read of the UNIQUE reap's reuse check fail
 * (xstore_reap_read_fail)?  One relaxed load when disarmed. */
static int
xs_reap_fault(void)
{
	if (xtc_atomic_load_i32(&g_reap_fail, xtc_mo_relaxed) == 0)
		return 0;
	(void)xtc_atomic_fetch_add_i32(&g_row_faults, 1, xtc_mo_relaxed);
	return 1;
}

int
xstore_reap_read_fail(int on)
{
	return xtc_atomic_exchange_i32(&g_reap_fail, on != 0, xtc_mo_relaxed);
}

int
xstore_row_read_faults(void)
{
	return xtc_atomic_load_i32(&g_row_faults, xtc_mo_relaxed);
}

/* Record that a read of table `tid` failed with rc, as the statement's
 * first read error unless it has one.  xstore_read_err() formats it as
 * "cannot read table T: <rc>"; the name is looked up there, since the
 * caller may hold the commit lock, which ranks after the cat lock. */
static void
xs_rd_err_tid(xstore_ctx_t *cx, uint32_t tid, int rc)
{
	if (cx == NULL || cx->rd_err[0] != '\0' || cx->rd_rc != 0)
		return;
	cx->rd_tid = tid;
	cx->rd_rc = rc;
}

/* Forget the statement's read error. */
static void
xs_rd_err_clear(xstore_ctx_t *cx)
{
	cx->rd_err[0] = '\0';
	cx->rd_rc = 0;
}

/* True if the running statement must not commit: one of its reads failed
 * (a failed read need not fail the step that made it: a subquery reads
 * as NULL), or a kill is pending for the connection's proc.  Every
 * autocommit commit of a statement checks it, the one-row direct write
 * and the implicit transaction alike, so a failed statement leaves
 * nothing on every path. */
static int
xs_stmt_doomed(const xstore_ctx_t *cx)
{
	return cx->rd_rc != 0 || cx->rd_err[0] != '\0' ||
	    xtc_cancel_requested();
}

uint64_t
xstore_read_errors(void)
{
	return xtc_atomic_load_u64(&g_rd_errors, xtc_mo_relaxed);
}

/* Open a cursor over catalog `cat` (from its first key) for a walk of
 * its entries; one read of the row-read fault hook.  XTC_OK or the
 * error, counted. */
static int
xs_cat_open(bt_t *bt, uint32_t cat, bt_cursor_t **cur)
{
	uint8_t startk[XS_VKLEN];
	int rc;
	enc_vkey(cat, INT64_MIN, ~(uint64_t)0, startk);
	rc = bt_cursor_open(bt, startk, XS_VKLEN, cur);
	if (rc == XTC_OK && xs_row_fault()) {
		bt_cursor_close(*cur);
		*cur = NULL;
		rc = XTC_E_IO;
	}
	return xs_rd_count(rc);
}

/* The next entry of a catalog walk: XTC_OK, XTC_E_NOTFOUND at the end of
 * the tree, or a read's error, counted.  One read of the row-read fault
 * hook per entry, so a test can fail a walk between entries. */
static int
xs_cat_next(bt_cursor_t *cur, const void **k, uint16_t *klen,
    const void **vv, uint16_t *vl)
{
	int rc = bt_cursor_next(cur, k, klen, vv, vl);
	if (rc == XTC_OK && xs_row_fault())
		rc = XTC_E_IO;
	return xs_rd_count(rc);
}

/* Look up `name` in the persisted catalog (table-id 0): the newest
 * non-tombstone catalog row whose value equals `name`.  Returns its id
 * (the row's rowid), or 0 if the catalog has no row for it; a read that
 * failed returns 0 with its XTC_E_* code in *err (the name may well be
 * taken: a caller must not create it or call it gone). */
static uint32_t
xs_cat_lookup(bt_t *bt, const char *name, int *err)
{
	bt_cursor_t *cur = NULL;
	size_t want = strlen(name);
	int64_t last_rowid = 0; int have_last = 0, rc;
	uint32_t found = 0;

	*err = 0;
	xtc_atomic_fetch_add_u64(&g_cat_scans, 1, xtc_mo_relaxed);
	if ((rc = xs_cat_open(bt, XS_CAT_TABLEID, &cur)) != XTC_OK) {
		*err = rc;
		return 0;
	}
	for (;;) {
		const void *k = NULL, *vv = NULL;
		uint16_t klen = 0, vl = 0;
		const uint8_t *kb, *vb;
		int64_t rid;
		rc = xs_cat_next(cur, &k, &klen, &vv, &vl);
		if (rc != XTC_OK && rc != XTC_E_NOTFOUND)
			*err = rc;
		if (rc != XTC_OK || klen != XS_VKLEN)
			break;
		kb = (const uint8_t *)k; vb = (const uint8_t *)vv;
		if (dec_tableid(kb) != XS_CAT_TABLEID)
			break;                       /* past the catalog */
		rid = dec_rowid(kb);
		if (have_last && rid == last_rowid)
			continue;                    /* older version of this id */
		last_rowid = rid; have_last = 1;
		if (vl >= 1 && (vb[0] & XS_F_DELETED))
			continue;                    /* a renamed-away/dropped entry */
		{
			/* The catalog value is the table name, optionally followed by
			 * a NUL and the serialized column schema.  Match the name
			 * portion (up to the NUL, or the whole value if none). */
			const uint8_t *nm = vb + 1;
			size_t nlen = (size_t)(vl >= 1 ? vl - 1 : 0), j;
			for (j = 0; j < nlen; j++) if (nm[j] == 0) { nlen = j; break; }
			if (nlen == want && strncasecmp((const char *)nm, name, want) == 0) {
				found = (uint32_t)rid;
				break;
			}
		}
	}
	bt_cursor_close(cur);
	return found;
}

/*
 * Install (bt, name) -> id with its column schema into the in-process
 * catalog cache after a catalog-TREE hit.  Before this, g_cat was filled
 * only by a CREATE in this process, so on any reopened store (every
 * server start) each statement resolved its table by scanning the
 * catalog B-tree, read the catalog value again for the schema, and
 * re-parsed it: ~19% of a TPC-C connection's CPU (xs_cat_lookup +
 * xs_cat_value + xstore_table_schema).  Entries are keyed and dropped
 * exactly as CREATE's are (DROP / RENAME / bt close), so a primed entry
 * is as authoritative as a created one.  Caller holds no cat lock.
 */
static int xs_cat_value(bt_t *bt, const char *name, uint8_t *out, int cap);
static void
xs_cat_prime(bt_t *bt, const char *name, uint32_t id)
{
	uint8_t val[1024];
	int vlen, i, j;
	if (id == 0 || strlen(name) >= sizeof g_cat[0].name) return;
	/* A cache fill: unread, nothing is cached. */
	vlen = xs_cat_value(bt, name, val, (int)sizeof val - 1);   /* RDERR_OK: fill */
	if (vlen < 0)
		return;      /* unread: a cache entry would lack the schema */
	xs_cat_lock();
	for (i = 0; i < g_cat_n; i++)
		if (g_cat[i].bt == bt && strcasecmp(g_cat[i].name, name) == 0) {
			xs_cat_unlock();
			return;                  /* raced: someone installed it */
		}
	if (g_cat_n < XS_CAT_MAX) {
		struct xs_cat_ent *e = &g_cat[g_cat_n++];
		e->bt = bt;
		snprintf(e->name, sizeof e->name, "%s", name);
		e->tableid = id;
		e->coldefs[0] = '\0';
		/* value = name [NUL coldefs]; cache the coldefs if they fit */
		for (j = 0; j < vlen; j++) if (val[j] == 0) break;
		if (j < vlen - 1 && vlen - (j + 1) < (int)sizeof e->coldefs) {
			memcpy(e->coldefs, val + j + 1, (size_t)(vlen - (j + 1)));
			e->coldefs[vlen - (j + 1)] = '\0';
		}
		XS_CATC_BUMP();
	}
	xs_cat_unlock();
}

static int xs_put(bt_t *bt, uint32_t tableid, int64_t rowid,
    const void *blob, int n, int deleted);   /* defined below */
static int xs_cat_high(bt_t *bt, uint32_t cat, uint32_t *out);   /* below */

/* Map (bt, name) to its allocated table-id: the in-process cache, then
 * the persisted catalog, else allocate a fresh dense id and persist it.
 * Returns the id (>= 1), or 0 if a new id could not be written or the
 * catalog could not be read; *rderr (if not NULL) is then the read's
 * XTC_E_* code, or 0 for a failed write.  *created (if not NULL) is set
 * to 1 when this call allocated the id. */
static uint32_t
xs_cat_find_or_create(bt_t *bt, const char *name, const char *coldefs,
    int *created, int *rderr)
{
	uint32_t id = 0, maxid = 0;
	int i, do_persist = 0, err;

	if (created != NULL) *created = 0;
	if (rderr != NULL) *rderr = 0;
	xs_cat_lock();
	for (i = 0; i < g_cat_n; i++)            /* seen in this process? */
		if (g_cat[i].bt == bt && strcasecmp(g_cat[i].name, name) == 0) {
			id = g_cat[i].tableid;
			xs_cat_unlock();
			return id;
		}
	/* Persisted (create/rename/recovery)?  An unread lookup or mark
	 * returns id 0, this function's failure. */
	id = xs_cat_lookup(bt, name, &err);   /* RDERR_OK: err: 0 */
	if (err == 0 && id == 0)
		/* New table: allocate past both the persisted high-water id
		 * and any id reserved in-process but not yet written. */
		err = xs_cat_high(bt, XS_CAT_TABLEID, &maxid);   /* RDERR_OK: err: 0 */
	if (err != 0) {                          /* unread: nothing known */
		xs_cat_unlock();
		if (rderr != NULL) *rderr = err;
		return 0;
	}
	if (id == 0) {
		for (i = 0; i < g_cat_n; i++)
			if (g_cat[i].bt == bt && g_cat[i].tableid > maxid)
				maxid = g_cat[i].tableid;
		id = maxid + 1u;
		if (id == 0u)
			id = 1u;                 /* 32-bit wrap guard (unreachable) */
		do_persist = 1;
	}
	if (g_cat_n < XS_CAT_MAX) {              /* reserve / shortcut next time */
		struct xs_cat_ent *e = &g_cat[g_cat_n++];
		XS_CATC_BUMP();
		e->bt = bt;
		snprintf(e->name, sizeof e->name, "%s", name);
		e->tableid = id;
		/* Cache the schema so a concurrent connection that resolves this
		 * table-id can read its columns immediately. */
		if (coldefs != NULL)
			snprintf(e->coldefs, sizeof e->coldefs, "%s", coldefs);
		else
			e->coldefs[0] = '\0';
	}

	/* Persist the new catalog row INSIDE the lock.  g_cat_mu is now an
	 * xtc_amutex, so xs_put may park on the WAL ack here without wedging
	 * the loop: a contending fiber simply parks behind us.  Persisting
	 * inside the lock means the durable catalog row exists before any
	 * other connection can acquire the lock and look the table up, which
	 * closes the cross-connection CREATE-then-SELECT race by
	 * construction (the in-process cache above is now an optimization,
	 * not the race fix).  The value is the table name, optionally
	 * followed by a NUL and the comma-joined column-def list, so the
	 * native schema reader can recover column names / types / the pk. */
	if (do_persist) {
		uint8_t vbuf[1024];
		size_t nlen = strlen(name), clen = coldefs ? strlen(coldefs) : 0;
		size_t total;
		int prc;
		if (nlen + 1 + clen > sizeof vbuf) clen = 0;   /* schema too long: name only */
		memcpy(vbuf, name, nlen);
		total = nlen;
		if (clen > 0) {
			vbuf[total++] = '\0';
			memcpy(vbuf + total, coldefs, clen);
			total += clen;
		}
		prc = xs_put(bt, XS_CAT_TABLEID, (int64_t)id, vbuf, (int)total, 0);
		xs_cat_unlock();
		if (prc != SX_OK)
			return 0;
		if (created != NULL) *created = 1;
		return id;
	}
	xs_cat_unlock();
	return id;
}

/* Drop every catalog-cache entry for `bt`.  Called from bt_close (via
 * the close hook) so a freed bt pointer reused by a new tree cannot
 * match a stale entry and resolve a wrong table-id.  The persisted
 * catalog (table-id 0) remains authoritative. */
static void xs_rowid_hw_forget(bt_t *bt);

static void xs_idx_dead_forget(bt_t *bt);
static void xs_idx_cache_forget_locked(bt_t *bt);
static void
xstore_cat_forget(bt_t *bt)
{
	int i;
	xs_rowid_hw_forget(bt);
	xs_idx_dead_forget(bt);
	xs_cat_lock();
	xs_idx_cache_forget_locked(bt);
	for (i = 0; i < g_cat_n; ) {
		if (g_cat[i].bt == bt) {
			g_cat_n--;
			if (i != g_cat_n) g_cat[i] = g_cat[g_cat_n];  /* swap-remove */
			XS_CATC_BUMP();
		} else {
			i++;
		}
	}
	xs_cat_unlock();
}

/* Forget the cached id for ONE (bt, name) -- used by xstore_drop_table
 * so a later create reallocates a fresh id rather than reusing the
 * dropped one from the in-process cache. */
static void
xstore_cat_forget_one(bt_t *bt, const char *name)
{
	int i;
	xs_cat_lock();
	for (i = 0; i < g_cat_n; i++)
		if (g_cat[i].bt == bt && strcasecmp(g_cat[i].name, name) == 0) {
			g_cat_n--;
			if (i != g_cat_n) g_cat[i] = g_cat[g_cat_n];
			XS_CATC_BUMP();
			break;
		}
	xs_cat_unlock();
}

/* ================================================================== *
 * Secondary indexes, UNIQUE constraints and composite PRIMARY KEYs
 *
 * An index is DERIVED data in the shared B-tree under its own key
 * namespace: one entry per (row version's key tuple, rowid),
 *
 *     [0xFE][idx_id:4 BE][elem]...[elem][rowid:8 offset-binary]
 *
 * with an empty value.  0xFE sorts after every table (table-id MSB 0x00)
 * and before the 0xFF staging / index-catalog namespaces.  Each elem is
 * an order-preserving encoding of one column value (xs_tup_elem), so
 * memcmp order over entries is SQLite's sort order over the tuple: an
 * equality prefix is a contiguous key range, a range on the next column
 * is a sub-range, and walking the range ascending yields ORDER BY the
 * index columns.  Entries are NOT WAL-logged.  A clean close leaves them
 * complete in the flushed base, and the next open trusts them (no
 * rebuild) when the superblock's trust tag says so: every index insert of
 * the run succeeded and the key format is XS_IDX_FORMAT (xstore_index_open).
 * Otherwise -- a crash, a failed insert, an older format -- every index is
 * rebuilt from the base rows.  Bump XS_IDX_FORMAT whenever the entry
 * encoding (xs_tup_elem, xs_idx_rowkey, the prefix) changes.  Format 2:
 * compact numbers (xs_tup_num).  Format 3: long keys (below).
 *
 * LONG KEYS.  A B-tree key holds at most bt_max_key() = K bytes (497 on
 * 4 KiB pages), and a key tuple is not bounded (a TEXT value takes its
 * length plus escapes).  Every row is indexed whatever its key's length:
 * a key tuple of at most P = K - 21 bytes is stored whole, a longer one
 * truncated, as STORAGE_DESIGN.md section 8.3 specifies:
 *     whole:     [0xFE][idx_id:4][key tuple, <= P bytes][rowid:8]
 *     truncated: [0xFE][idx_id:4][first P bytes of the key tuple]
 *                    [SipHash-1-3 of the whole key tuple: 8][rowid:8]
 * The hash is keyed by the index's (k0, k1), random at CREATE INDEX and
 * kept in its catalog row (0, 0 for an index made before format 3); it
 * covers the tuple bytes after the 5-byte prefix and is stored big-endian.
 * A truncated entry is exactly K bytes long and a whole one at most K - 8,
 * so the length tells them apart (xs_idx_truncated).
 * Order is exact except among truncated entries that share their first
 * P tuple bytes (a "group"): the tuple encoding is self-delimiting, so a
 * whole key differs from every other key within its own length, and
 * only a group sorts by hash.  The consequences, all handled here:
 *   - an entry match is a candidate, never an answer: a reader re-derives
 *     the entry from the row (as for stale entries), the planner's
 *     caller re-applies the WHERE, and UNIQUE compares the WHOLE tuples
 *     of a truncated match (xs_uniq_same);
 *   - a scan bound longer than P is widened to its P-byte group (with the
 *     hash when every column is bound) and the rows decide;
 *   - an index scan re-sorts each group by whole key (xs_iscan_group),
 *     so index order, MIN/MAX and LIMIT stay exact.
 * The HASH index (STORAGE_DESIGN.md section 10) relies on the same
 * tuple encoding and the same keyed SipHash-1-3 over it.
 *
 * Entries are ADD-ONLY at commit: a row version whose key tuple differs
 * from its predecessor adds an entry, and the predecessor's entry is
 * queued and reaped once the GC horizon passes the commit (no snapshot
 * can still need it).  Readers therefore see a superset of the right
 * candidates and verify each one: the visible version of the candidate
 * row is fetched and its key tuple re-encoded and compared with the
 * entry's -- a stale entry (the row's visible key is elsewhere) is
 * skipped, an old snapshot still finds the old key.
 *
 * A UNIQUE index (CREATE UNIQUE INDEX, a UNIQUE column / table
 * constraint, or a PRIMARY KEY that is not a rowid alias -- SQLite's
 * sqlite_autoindex_<t>_<n>) is enforced at statement time against the
 * committed index and the transaction's own writes, and re-validated at
 * commit under the commit lock (xs_uniq_validate_locked).  A key with a
 * NULL element never conflicts (SQLite).
 *
 * The index CATALOG is persisted at reserved table-id XS_IDXCAT_TABLEID
 * as ordinary version rows (WAL-durable): one row per index, rowid =
 * dense index id, value
 *     "<name>\0<table>\0<c1,c2,...>\0<unique>\0<descmask>\0<kind>\0
 *      <k0>,<k1>"
 * kind 'p' = PRIMARY KEY autoindex, 'u' = UNIQUE constraint autoindex,
 * 'i' = CREATE INDEX; k0, k1 the SipHash key in hex (absent before 2.2:
 * 0, 0).  A 1.57-format row ("<name>\0<table>\0<col>\0<u>") still
 * parses as a one-column ascending CREATE INDEX.
 * ================================================================== */
#define XS_IDXCAT_TABLEID 0xFFFFFFFDu   /* persisted index catalog */
#define XS_IDX_FORMAT     3u            /* index entry encoding version (1: 1.59) */
/* The superblock word a clean close records when the indexes are complete
 * ("XIDX" + the format); xstore_index_open trusts only this value. */
#define XS_IDX_TRUST_TAG  ((0x58494458ull << 32) | XS_IDX_FORMAT)
#define XS_IDX_LEAD       0xFEu         /* index-entry key namespace lead byte */
#define XS_IDX_MAX        128           /* in-process index cache size */
#define XS_IDX_NCOL       16            /* columns per index */
#define XS_IDX_KMAX       1024          /* >= BT_MAX_KEY on every page size */
/* The longest key tuple a record of XS_VMAX bytes encodes: each payload
 * byte escaped once (TEXT/BLOB 0x00 -> 0x00 0xFF), plus at most 32 bytes
 * of tags, terminators and number bodies per column, and the prefix. */
#define XS_IDX_TMAX       (5 + 2 * XS_VMAX + 32 * XS_IDX_NCOL)

/* Element type tags, in SQLite's cross-class order.  A number's tag is
 * XS_TUP_NUM plus (minus, if negative) its magnitude class 0..9
 * (xs_tup_num), so numbers take 0x0C..0x1E, between NULL and TEXT. */
#define XS_TUP_NULL 0x05u
#define XS_TUP_NUM  0x15u
#define XS_TUP_TEXT 0x25u
#define XS_TUP_BLOB 0x35u
#define XS_TUP_HUGE 9          /* magnitude class of |v| >= 2^63 */

/* The runtime part of an index: what key encoding and maintenance
 * need, resolved once when the index enters the cache. */
struct xs_idx_rt {
	uint32_t idx_id;
	uint32_t tableid;
	int      ncol;
	int      pay[XS_IDX_NCOL];  /* payload index per column; -1 = rowid */
	uint32_t descmask;          /* bit i: column i is DESC */
	int      unique;
	char     kind;              /* 'p' / 'u' / 'i' */
	int      kmax;              /* bt_max_key: the entry bound */
	uint64_t k0, k1;            /* SipHash key of truncated entries */
	int      broken;            /* XTC_E_* of a failed rebuild, or 0 */
};
struct xs_idx_ent {
	bt_t    *bt;
	struct xs_idx_rt rt;
	char     name[64];
	char     table[64];
	char     col[XS_IDX_NCOL][64];
};
static struct xs_idx_ent g_idx[XS_IDX_MAX];
/* g_idx[] and g_idx_n change only under the cat lock.  The "any index?"
 * fast paths read g_idx_n without it, so every write of g_idx_n is a
 * release store (XS_IDX_N_SET, after the entry it exposes is written) and
 * every unlocked read an acquire load (XS_IDX_ANY).  Only the count is read
 * without the lock; the entries are always read under it. */
static int g_idx_n;
#define XS_IDX_N_SET(v) xtc_atomic_store_i32(&g_idx_n, (v), xtc_mo_release)
#define XS_IDX_ANY() (xtc_atomic_load_i32(&g_idx_n, xtc_mo_acquire) > 0)
static uint64_t g_idx_gen; /* bumped by every index create */
/* Set when an index entry may be missing (an insert failed, a build ran
 * short, an index is not maintained): the indexes are then not trusted at
 * the next open.  Cleared by xstore_index_open.  shortcut: one flag for
 * the process (one storage engine per process); per-bt if that changes. */
static int g_idx_taint;
static uint64_t g_idx_rebuilds; /* full index rebuilds (metric) */
/* Superseded entries left in the tree because the dead queue could not
 * hold or reap them, carried across trusted reopens in the superblock.
 * Stale entries are harmless but cost readers a fetch each, so a reopen
 * rebuilds once they pass XS_IDX_GARBAGE_MAX. */
static uint64_t g_idx_garbage;
#define XS_IDX_GARBAGE_MAX (1u << 16)
static int g_idx_failinj; /* test hook: inserts to fail */
static int g_uniq_failinj; /* test hook: UNIQUE probes to fail */
static int g_rd_failat;    /* test hook: the index read to fail, 1-based */
static int g_rd_faults;    /* test hook: reads it has failed */

/* Catalog generation: bumped by every DDL entry point (tables, indexes,
 * views) and index rebuild, so a cached plan built at an older one is
 * rebuilt (it may name a dropped index or miss a new one). */
static uint64_t g_cat_gen = 1;

uint64_t
xstore_cat_scan_count(void)
{
	return xtc_atomic_load_u64(&g_cat_scans, xtc_mo_relaxed);
}

uint64_t
xstore_cat_lock_count(void)
{
	return xtc_atomic_load_u64(&g_cat_locks, xtc_mo_relaxed);
}

uint64_t
xstore_catalog_gen(void)
{
	return xtc_atomic_load_u64(&g_cat_gen, xtc_mo_acquire);
}

static void
xs_cat_gen_bump(void)
{
	xtc_atomic_fetch_add_u64(&g_cat_gen, 1, xtc_mo_acq_rel);
}

/* Caller holds the cat lock: drop every cached index of a closing bt. */
static void
xs_idx_cache_forget_locked(bt_t *bt)
{
	int i;
	for (i = 0; i < g_idx_n; ) {
		if (g_idx[i].bt == bt) {
			g_idx[i] = g_idx[g_idx_n - 1];
			XS_IDX_N_SET(g_idx_n - 1);
			XS_CATC_BUMP();
		}
		else i++;
	}
}
static uint64_t g_idx_seeks; /* index range seeks served (metric) */

/* The (idx, rowid, key) of a superseded index entry, reaped once no
 * snapshot at or before `ts` can be live. */
struct xs_idx_dead {
	bt_t    *bt;
	uint64_t ts;
	uint32_t idx_id;
	uint32_t tableid;
	int64_t  rowid;
	uint16_t klen;
	uint8_t *key;
};
#define XS_IDX_DEAD_MAX 4096
static struct xs_idx_dead g_idx_dead[XS_IDX_DEAD_MAX];
static int g_idx_dead_n;      /* guarded by the commit lock */
/* Its value for the unlocked "anything queued?" fast paths (a hint: the
 * locked code re-reads g_idx_dead_n).  Written under the commit lock. */
static int g_idx_dead_hint;
#define XS_IDX_DEAD_PUBLISH()                                                  \
	xtc_atomic_store_i32(&g_idx_dead_hint, g_idx_dead_n, xtc_mo_relaxed)
#define XS_IDX_DEAD_HINT() xtc_atomic_load_i32(&g_idx_dead_hint, xtc_mo_relaxed)

static void
xs_put_be64(uint8_t *p, uint64_t v)
{
	int i;
	for (i = 7; i >= 0; i--) { p[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}

static uint64_t
xs_get_be64(const uint8_t *p)
{
	uint64_t v = 0;
	int i;
	for (i = 0; i < 8; i++) v = (v << 8) | p[i];
	return v;
}

/*
 * The order-preserving element of a number (INTEGER, or REAL: 3 and 3.0
 * are one key, and a mixed column orders by exact value, as in SQLite).
 * With a = |v| < 2^63, m = 2 * floor(a) + (a has a fraction): the tag is
 * XS_TUP_NUM + n for the n = 0..8 big-endian bytes of m (no leading zero
 * byte; zero is the bare tag), then the fraction's 8 IEEE-754 bytes when
 * m is odd.  |v| >= 2^63 (only a REAL, or INT64_MIN) is class 9: the 8
 * bytes of |v|.  A negative number takes XS_TUP_NUM - class and the
 * complement of its magnitude's bytes, so a larger magnitude sorts first.
 * Within a class the bytes order by m and then the fraction, and an
 * element's length follows from its tag and m's low bit, so no element
 * is a prefix of another (the scans compare a bound as a byte prefix).
 * Integers below 64 take 2 bytes, below 2^15 3 bytes, a microsecond
 * timestamp 8: the old form was 11 bytes for every number (an 8-byte
 * sign-flipped double plus a 2-byte residual for integers beyond 2^53),
 * and gave INT64_MAX and the REAL 2^63 the same key -- a false UNIQUE
 * conflict.  Returns the new offset, or -1 if it does not fit in `cap`.
 */
static int
xs_tup_num(uint8_t *out, int off, int cap, int cls, int64_t iv, double rv)
{
	uint8_t body[16];
	uint64_t m = 0, u;
	double a, f = 0;
	int neg, k, nb = 0, i;

	if (cls == XSTORE_C_INT && iv != INT64_MIN) {
		neg = iv < 0;
		m = 2u * (neg ? 0u - (uint64_t)iv : (uint64_t)iv);
		k = 0;
	} else {
		a = cls == XSTORE_C_INT ? (double)iv : rv;   /* INT64_MIN: exact */
		neg = a < 0;                                  /* NaN: not negative */
		if (neg) a = -a;
		if (a < 9223372036854775808.0) {              /* false for NaN */
			u = (uint64_t)a;
			f = a - (double)u;           /* exact: u is a's integer part */
			m = 2u * u + (f != 0);
			k = 0;
		} else {
			k = XS_TUP_HUGE;
			memcpy(&u, &a, 8);               /* a >= 0: bits order as values */
			for (i = 7; i >= 0; i--) body[nb++] = (uint8_t)(u >> (8 * i));
		}
	}
	if (k == 0) {
		while (k < 8 && (m >> (8 * k)) != 0)
			k++;
		for (i = k - 1; i >= 0; i--) body[nb++] = (uint8_t)(m >> (8 * i));
		if (m & 1u) {
			memcpy(&u, &f, 8);               /* 0 < f < 1 */
			for (i = 7; i >= 0; i--) body[nb++] = (uint8_t)(u >> (8 * i));
		}
	}
	if (off + 1 + nb > cap) return -1;
	out[off++] = (uint8_t)(neg ? XS_TUP_NUM - (unsigned)k : XS_TUP_NUM + (unsigned)k);
	for (i = 0; i < nb; i++)
		out[off++] = neg ? (uint8_t)~body[i] : body[i];
	return off;
}

/*
 * Append one order-preserving tuple element for a value of storage
 * class `cls` (XSTORE_C_*): a number as xs_tup_num.  TEXT and BLOB
 * escape 0x00 as 0x00 0xFF and end with 0x00 0x00, so a value is never
 * a byte prefix of a longer one ("n1" < "n10").  `desc` complements the
 * element's bytes.  Returns the new offset, or -1 if the element does
 * not fit in `cap`.
 */
static int
xs_tup_elem(uint8_t *out, int off, int cap, int cls, int64_t iv, double rv,
    const uint8_t *p, int n, int desc)
{
	int start = off, i;
	if (off < 0) return -1;
	switch (cls) {
	case XSTORE_C_INT: case XSTORE_C_REAL:
		if ((off = xs_tup_num(out, off, cap, cls, iv, rv)) < 0)
			return -1;
		break;
	case XSTORE_C_TEXT: case XSTORE_C_BLOB:
		if (off + 1 > cap) return -1;
		out[off++] = cls == XSTORE_C_TEXT ? XS_TUP_TEXT : XS_TUP_BLOB;
		for (i = 0; i < n; i++) {
			if (off + 2 > cap) return -1;
			out[off++] = p[i];
			if (p[i] == 0) out[off++] = 0xFF;
		}
		if (off + 2 > cap) return -1;
		out[off++] = 0; out[off++] = 0;
		break;
	default:
		if (off + 1 > cap) return -1;
		out[off++] = XS_TUP_NULL;
		break;
	}
	if (desc)
		for (i = start; i < off; i++) out[i] = (uint8_t)~out[i];
	return off;
}

int
xstore_index_elem(uint8_t *out, int cap, const xstore_val_t *v, int desc)
{
	return xs_tup_elem(out, 0, cap, v->cls, v->i, v->r, v->p, v->n, desc);
}

/* The 5-byte namespace prefix of index `idx_id`. */
static int
xs_idx_prefix(uint32_t idx_id, uint8_t *out)
{
	int i;
	out[0] = XS_IDX_LEAD;
	for (i = 3; i >= 0; i--) out[4 - i] = (uint8_t)((idx_id >> (8 * i)) & 0xFF);
	return 5;
}

/* Resolve index columns `col[0..ncol)` of `table` to payload positions
 * (0-based over the non-rowid columns; -1 = the rowid).  Returns 0, -1
 * if the table or a column is unknown, or the XTC_E_* code (below -1)
 * of a catalog read that failed. */
static int
xs_idx_resolve(bt_t *bt, const char *table, char col[][64], int ncol, int *pay)
{
	xstore_col_t cols[64];
	int n, i, c;
	n = xstore_table_schema(bt, table, cols, 64);
	if (n < -1) return n;                /* unread */
	if (n <= 0) return -1;
	for (i = 0; i < ncol; i++) {
		pay[i] = -2;
		for (c = 0; c < n; c++)
			if (strcmp(cols[c].name, col[i]) == 0) {
				pay[i] = c == 0 ? -1 : c - 1;
				break;
			}
		if (pay[i] == -2) return -1;
	}
	return 0;
}

/*
 * Encode the whole key tuple of a row: the namespace prefix and one
 * element per index column read from payload record `rec` (rowid for
 * pay == -1), never truncated.  *has_null is set when an element is
 * NULL (exempt from UNIQUE).  Returns the length, or -1 if it exceeds
 * `cap` (XS_IDX_TMAX holds any record's).
 */
static int
xs_idx_tuple(const struct xs_idx_rt *e, const uint8_t *rec, int reclen,
    int64_t rowid, uint8_t *out, int cap, int *has_null)
{
	/* The record is decoded once (xstore_rec_cols), up to the highest
	 * payload column the index names; xstore_rec_col per column
	 * re-parsed it from the start for each one. */
	xstore_val_t cv[XS_IDX_NCOL];
	int off, i, need = 0, got;
	if (has_null) *has_null = 0;
	for (i = 0; i < e->ncol; i++)
		if (e->pay[i] >= need) need = e->pay[i] + 1;
	got = need <= XS_IDX_NCOL ? xstore_rec_cols(rec, reclen, cv, need) : -1;
	off = xs_idx_prefix(e->idx_id, out);
	for (i = 0; i < e->ncol && off >= 0; i++) {
		int64_t iv = 0; double rv = 0; const uint8_t *p = NULL; int np = 0;
		int cls;
		if (e->pay[i] < 0) { cls = XSTORE_C_INT; iv = rowid; }
		else if (got < 0)
			cls = xstore_rec_col(rec, reclen, e->pay[i], &iv, &rv, &p, &np);
		else if (e->pay[i] >= got)
			cls = XSTORE_C_NULL;      /* as xstore_rec_col past the end */
		else {
			const xstore_val_t *x = &cv[e->pay[i]];
			cls = x->cls; iv = x->i; rv = x->r; p = x->p; np = x->n;
		}
		if (cls == XSTORE_C_NULL && has_null) *has_null = 1;
		off = xs_tup_elem(out, off, cap, cls, iv, rv, p, np,
		    (int)((e->descmask >> i) & 1u));
	}
	return off;
}

/* Is an entry key of `klen` bytes (with its rowid suffix) truncated? */
static int
xs_idx_truncated(const struct xs_idx_rt *e, int klen)
{
	return klen == e->kmax;
}

/*
 * The entry form of whole key tuple tup[0..tl) (prefix included): the
 * tuple itself if it holds at most P = kmax - 21 bytes after the
 * prefix, else its first P bytes and the SipHash-1-3 of all of them
 * (the encoding in the LONG KEYS comment above), then (with_rowid) the
 * rowid.  Returns the length (at most kmax), or -1 if `cap` is short or
 * the page is too small to hold a truncated entry.
 */
static int
xs_idx_entkey(const struct xs_idx_rt *e, const uint8_t *tup, int tl,
    int64_t rowid, int with_rowid, uint8_t *out, int cap)
{
	int p = e->kmax - 21, n;
#if XTC_DST_BUG(XTC_DST_BUG_LONGKEY)
	if (with_rowid && tl - 5 > p)
		return -1; /* planted 73: a long key gets no index entry */
#endif
	/* shortcut: pages under 512 bytes leave no room for a truncated
	 * entry and do not index a long key; sqlxtc uses 4 KiB and up. */
	if (p < 1)
		return -1;
	n = tl - 5 <= p ? tl : 5 + p + 8;
	if (n + (with_rowid ? 8 : 0) > cap)
		return -1;
	if (tl - 5 <= p) {
		memcpy(out, tup, (size_t)tl);
	} else {
		memcpy(out, tup, (size_t)(5 + p));
		xs_put_be64(out + 5 + p,
		    sx_siphash13(e->k0, e->k1, tup + 5, (size_t)(tl - 5)));
	}
	if (with_rowid) {
		xs_put_be64(out + n, (uint64_t)rowid ^ 0x8000000000000000ull);
		n += 8;
	}
	return n;
}

/*
 * Encode the index entry key of a row (xs_idx_tuple, then the entry
 * form, xs_idx_entkey) into `out` (cap >= kmax), with the 8-byte rowid
 * suffix if with_rowid.  Every row has one: returns the length, or -1
 * only on a short `cap` or a page too small for long keys.
 */
static int
xs_idx_rowkey(const struct xs_idx_rt *e, const uint8_t *rec, int reclen,
    int64_t rowid, int with_rowid, uint8_t *out, int cap, int *has_null)
{
	/* Per thread, not on the fiber stack: nothing here parks. */
	static _Thread_local uint8_t tup[XS_IDX_TMAX];
	int tl;
	tl =
	    xs_idx_tuple(e, rec, reclen, rowid, tup, (int)sizeof tup, has_null);
	if (tl < 0)
		return -1;
	return xs_idx_entkey(e, tup, tl, rowid, with_rowid, out, cap);
}

/* Do two rows carry one whole key tuple in index `e`?  For a truncated
 * entry match, which the hash alone cannot prove. */
static int
xs_idx_tuple_eq(const struct xs_idx_rt *e, const uint8_t *ra, int la,
    int64_t ida, const uint8_t *rb, int lb, int64_t idb)
{
	static _Thread_local uint8_t ta[XS_IDX_TMAX], tb[XS_IDX_TMAX];
	int na, nb;
	na = xs_idx_tuple(e, ra, la, ida, ta, (int)sizeof ta, NULL);
	nb = xs_idx_tuple(e, rb, lb, idb, tb, (int)sizeof tb, NULL);
	return na >= 0 && na == nb && memcmp(ta, tb, (size_t)na) == 0;
}

/* Read the newest committed record for (tableid, rowid) into `rec`
 * (capacity `cap`).  Returns the record length (>= 1) on a live row, 0
 * if absent / tombstoned, or a negative XTC_E_* code if the tree could
 * not be read.  Used to fetch a row's pre-image for index maintenance
 * and a UNIQUE candidate's key. */
static int
xs_read_rec(bt_t *bt, uint32_t tableid, int64_t rowid, uint8_t *rec, int cap)
{
	bt_cursor_t *cur = NULL;
	uint8_t startk[XS_VKLEN];
	const void *k = NULL, *vv = NULL;
	uint16_t klen = 0, vl = 0;
	int out = 0, rc;
	enc_vkey(tableid, rowid, ~(uint64_t)0, startk);   /* newest first */
	if ((rc = bt_cursor_open(bt, startk, XS_VKLEN, &cur)) != XTC_OK)
		return rc;
	rc = bt_cursor_next(cur, &k, &klen, &vv, &vl);
	if (rc != XTC_OK && rc != XTC_E_NOTFOUND)
		out = rc;
	else if (rc == XTC_OK && klen == XS_VKLEN &&
	    dec_tableid((const uint8_t *)k) == tableid &&
	    dec_rowid((const uint8_t *)k) == rowid && vl >= 1 &&
	    !(((const uint8_t *)vv)[0] & XS_F_DELETED)) {
		int n = (int)vl - 1;   /* strip the flag byte */
		if (n > cap) n = cap;
		if (n > 0) memcpy(rec, (const uint8_t *)vv + 1, (size_t)n);
		out = n;
	}
	bt_cursor_close(cur);
	return out;
}

/* All indexes on `tableid` (runtime descriptors, copied under the cat
 * lock).  Returns the count.  The g_idx_n == 0 fast path makes the
 * per-write hook free when no index exists. */
static int
xs_idx_for_table(bt_t *bt, uint32_t tableid, struct xs_idx_rt *hits, int cap)
{
	static _Thread_local struct {
		uint64_t gen; bt_t *bt; uint32_t tid; int n;
		struct xs_idx_rt rt[16];
	} tc[4];
	static _Thread_local unsigned tnext;
	uint64_t g;
	int n = 0, i, k;
	if (!XS_IDX_ANY()) return 0;
	g = xtc_atomic_load_u64(&g_catc_gen, xtc_mo_acquire);
	for (k = 0; k < 4; k++)
		if (tc[k].gen == g && tc[k].bt == bt && tc[k].tid == tableid &&
		    tc[k].n <= cap) {
			memcpy(hits, tc[k].rt, sizeof hits[0] * (size_t)tc[k].n);
			return tc[k].n;
		}
	xs_cat_lock();
	g = xtc_atomic_load_u64(&g_catc_gen, xtc_mo_relaxed);
	for (i = 0; i < g_idx_n && n < cap; i++)
		if (g_idx[i].bt == bt && g_idx[i].rt.tableid == tableid)
			hits[n++] = g_idx[i].rt;
	xs_cat_unlock();
	if (n <= 16) {
		k = (int)(tnext++ % 4u);
		tc[k].gen = g; tc[k].bt = bt; tc[k].tid = tableid; tc[k].n = n;
		memcpy(tc[k].rt, hits, sizeof hits[0] * (size_t)n);
	}
	return n;
}

/* Queue a superseded entry of index `rt` for row `rowid` for reaping
 * after `ts` leaves every snapshot.  A full queue drops it (the entry
 * stays: readers verify, so a lingering entry costs a candidate fetch,
 * never a wrong row). */
static void
xs_idx_dead_add(bt_t *bt, const struct xs_idx_rt *rt, int64_t rowid,
    uint64_t ts, const uint8_t *key, int klen)
{
	uint8_t *k;
	if (klen <= 0) return;
	if (XS_IDX_DEAD_HINT() >= XS_IDX_DEAD_MAX ||
	    (k = (uint8_t *)malloc((size_t)klen)) == NULL) {
		xtc_atomic_fetch_add_u64(&g_idx_garbage, 1, xtc_mo_relaxed);
		return;
	}
	memcpy(k, key, (size_t)klen);
	xs_commit_lock();
	if (g_idx_dead_n < XS_IDX_DEAD_MAX) {
		struct xs_idx_dead *d = &g_idx_dead[g_idx_dead_n++];
		XS_IDX_DEAD_PUBLISH();
		d->bt = bt; d->ts = ts; d->idx_id = rt->idx_id;
		d->tableid = rt->tableid; d->rowid = rowid;
		d->klen = (uint16_t)klen; d->key = k;
		k = NULL;
	}
	xs_commit_unlock();
	if (k != NULL)                     /* lost the race for the last slot */
		xtc_atomic_fetch_add_u64(&g_idx_garbage, 1, xtc_mo_relaxed);
	free(k);
}

static int xs_idx_get_rt(bt_t *bt, uint32_t idx_id, struct xs_idx_rt *rt);
static int xs_inflight_writes(uint32_t tid, int64_t rid);

/* Does any version of (rt->tableid, rowid) committed at or after `ts`
 * carry index key `key` (with rowid suffix)?  Such a version (re)uses
 * the entry, so it must survive the reap.  A read that fails answers
 * "used" (counted): a stale entry costs a candidate fetch, a missing one
 * lets a duplicate UNIQUE value in. */
static int
xs_idx_key_reused(bt_t *bt, const struct xs_idx_rt *rt, int64_t rowid,
    uint64_t ts, const uint8_t *key, int klen)
{
	bt_cursor_t *cur = NULL;
	uint8_t startk[XS_VKLEN], kk[XS_IDX_KMAX];
	int used = 0, kl, rc;
	enc_vkey(rt->tableid, rowid, ~(uint64_t)0, startk);
	rc = bt_cursor_open(bt, startk, XS_VKLEN, &cur);   /* RDERR_OK: "used" */
	if (rc != XTC_OK) {
		(void)xs_rd_count(rc);
		return 1;
	}
	for (;;) {
		const void *k = NULL, *vv = NULL; uint16_t kln = 0, vl = 0;
		const uint8_t *kb;
		rc = bt_cursor_next(cur, &k, &kln, &vv, &vl);   /* RDERR_OK: "used" */
		if (rc == XTC_OK && (xs_row_fault() || xs_reap_fault()))
			rc = XTC_E_IO;
		if (rc != XTC_OK && rc != XTC_E_NOTFOUND) {
			(void)xs_rd_count(rc);
			used = !XTC_DST_BUG(XTC_DST_BUG_REAPREAD); /* planted 86 */
			break;
		}
		if (rc != XTC_OK || kln != XS_VKLEN)
			break;
		kb = (const uint8_t *)k;
		if (dec_tableid(kb) != rt->tableid || dec_rowid(kb) != rowid ||
		    dec_ts(kb) < ts)
			break;
		if (vl < 1 || (((const uint8_t *)vv)[0] & XS_F_DELETED)) continue;
		kl = xs_idx_rowkey(rt, (const uint8_t *)vv + 1, (int)vl - 1, rowid, 1,
		    kk, (int)sizeof kk, NULL);
		if (kl == klen && memcmp(kk, key, (size_t)kl) == 0) { used = 1; break; }
	}
	bt_cursor_close(cur);
	return used;
}

/* Reap queued entries whose superseding commit is at or below the GC
 * horizon (no live snapshot predates it).  Each entry is re-checked and
 * deleted under the commit lock: an entry is deleted only if no commit
 * of its row is in flight and no version at or after the queued commit
 * carries the key -- so a later version that reuses the key (or a
 * writer that is adding it right now) keeps it. */
static void
xs_idx_reap(bt_t *bt)
{
	struct xs_idx_dead batch[32];
	struct xs_idx_rt rt;
	uint64_t hz;
	int i, nb = 0;
	if (XS_IDX_DEAD_HINT() == 0) return;
	hz = snap_horizon(xs_visible());
	xs_commit_lock();
	for (i = 0; i < g_idx_dead_n && nb < 32; ) {
		if (g_idx_dead[i].bt == bt && g_idx_dead[i].ts <= hz) {
			batch[nb++] = g_idx_dead[i];
			g_idx_dead[i] = g_idx_dead[--g_idx_dead_n];
		} else {
			i++;
		}
	}
	XS_IDX_DEAD_PUBLISH();
	xs_commit_unlock();
	for (i = 0; i < nb; i++) {
		struct xs_idx_dead *d = &batch[i];
		int keep;
		if (!xs_idx_get_rt(bt, d->idx_id, &rt)) {
			free(d->key);                 /* index dropped: cleared already */
			continue;
		}
		/* Under the commit lock: a committer registers (under it) before
		 * it adds its entries, so none can add this key between the check
		 * and the delete; and a closed checkpoint gate means the tree must
		 * stay still -- requeue. */
		xs_commit_lock();
		if (g_ckpt_gate || xs_inflight_writes(d->tableid, d->rowid)) {
			xs_commit_unlock();
			xs_idx_dead_add(bt, &rt, d->rowid, d->ts, d->key, d->klen);
			free(d->key);
			continue;                     /* retry on a later reap */
		}
		/* A read error answers "used": the entry is kept. */
		keep = xs_idx_key_reused(bt, &rt, d->rowid, /* RDERR_OK: kept */
		    d->ts, d->key, d->klen);
		if (!keep)
			(void)bt_delete(bt, d->key, d->klen);
		xs_commit_unlock();
		free(d->key);
	}
}

/* Clean close: reap every queued entry of `bt` that no snapshot needs, so
 * the trusted base carries no queued garbage (the queue lives in memory).
 * Commits are stopped.  An entry a pinned snapshot or an in-flight commit
 * still needs stays in the tree: stale, which readers filter. */
static void
xs_idx_drain(bt_t *bt)
{
	struct xs_idx_dead d;
	struct xs_idx_rt rt;
	uint64_t hz = snap_horizon(xs_visible());
	int i;
	for (;;) {
		xs_commit_lock();
		for (i = 0; i < g_idx_dead_n; i++)
			if (g_idx_dead[i].bt == bt) break;
		if (i == g_idx_dead_n) { xs_commit_unlock(); break; }
		d = g_idx_dead[i];
		g_idx_dead[i] = g_idx_dead[--g_idx_dead_n];
		XS_IDX_DEAD_PUBLISH();
		xs_commit_unlock();
		if (!xs_idx_get_rt(bt, d.idx_id, &rt)) {
			free(d.key);               /* index dropped: cleared already */
			continue;
		}
		xs_commit_lock();
		if (d.ts > hz || xs_inflight_writes(d.tableid, d.rowid))
			xtc_atomic_fetch_add_u64(&g_idx_garbage, 1,
			    xtc_mo_relaxed); /* a snapshot still needs it */
		/* A read error answers "used": the entry is kept. */
		else if (!xs_idx_key_reused(bt, &rt, /* RDERR_OK: kept */
		    d.rowid, d.ts, d.key, d.klen))
			(void)bt_delete(bt, d.key, d.klen);
		xs_commit_unlock();
		free(d.key);
	}
}

/* Forget queued entries of a closing tree (they stay in the tree). */
static void
xs_idx_dead_forget(bt_t *bt)
{
	int i;
	xs_commit_lock();
	for (i = 0; i < g_idx_dead_n; ) {
		if (g_idx_dead[i].bt == bt) {
			xtc_atomic_fetch_add_u64(&g_idx_garbage, 1,
			    xtc_mo_relaxed);
			free(g_idx_dead[i].key);
			g_idx_dead[i] = g_idx_dead[--g_idx_dead_n];
		} else {
			i++;
		}
	}
	XS_IDX_DEAD_PUBLISH();
	xs_commit_unlock();
}

/* Insert one index entry.  A failure means the index misses a row until
 * it is rebuilt, so it taints the indexes (see g_idx_taint). */
static int
xs_idx_put(bt_t *bt, const uint8_t *k, int kl)
{
	int rc;
	if (xtc_atomic_load_i32(&g_idx_failinj, xtc_mo_relaxed) > 0 &&
	    xtc_atomic_fetch_sub_i32(&g_idx_failinj, 1, xtc_mo_relaxed) > 0)
		rc = XTC_E_NOMEM;              /* xstore_index_fail_inject */
	else
		rc = bt_insert(bt, k, (uint16_t)kl, "", 0);
	if (rc != XTC_OK)
		xtc_atomic_store_i32(&g_idx_taint, 1, xtc_mo_release);
	return rc;
}

void
xstore_index_fail_inject(int n)
{
	xtc_atomic_store_i32(&g_idx_failinj, n, xtc_mo_relaxed);
}

void
xstore_uniq_fail_inject(int n)
{
	xtc_atomic_store_i32(&g_uniq_failinj, n, xtc_mo_relaxed);
}

int
xstore_index_read_fail_at(int n)
{
	return xtc_atomic_exchange_i32(&g_rd_failat, n, xtc_mo_relaxed);
}

int
xstore_index_read_faults(void)
{
	return xtc_atomic_load_i32(&g_rd_faults, xtc_mo_relaxed);
}

/* Should this B-tree read of an index scan or a UNIQUE probe fail
 * (xstore_index_read_fail_at)?  One relaxed load when disarmed. */
static int
xs_rd_fault(void)
{
	if (xtc_atomic_load_i32(&g_rd_failat, xtc_mo_relaxed) <= 0 ||
	    xtc_atomic_fetch_sub_i32(&g_rd_failat, 1, xtc_mo_relaxed) != 1)
		return 0;
	(void)xtc_atomic_fetch_add_i32(&g_rd_faults, 1, xtc_mo_relaxed);
	return 1;
}

/* Should this UNIQUE probe fail (xstore_uniq_fail_inject)? */
static int
xs_uniq_fail_now(void)
{
	return xtc_atomic_load_i32(&g_uniq_failinj, xtc_mo_relaxed) > 0 &&
	    xtc_atomic_fetch_sub_i32(&g_uniq_failinj, 1, xtc_mo_relaxed) > 0;
}

/*
 * Index maintenance for one committed base-row version (delete == 1 for
 * a tombstone), called after the version is in the tree.  oldrec is the
 * row's prior committed record (captured before the write) or NULL.
 * The new key tuple's entry is added unless it equals the old one; the
 * old entry is queued for reaping at `ts`.  A MISSING entry would be a
 * false negative, so the add runs on the path that makes the base row
 * visible.
 */
static void
xs_idx_maintain_ts(bt_t *bt, uint32_t tableid, int64_t rowid,
    const uint8_t *newrec, int newlen, int deleted,
    const uint8_t *oldrec, int oldlen, uint64_t ts)
{
	struct xs_idx_rt hits[17];
	uint8_t nk[XS_IDX_KMAX], ok[XS_IDX_KMAX];
	int nh, i, nkl, okl;
	nh = xs_idx_for_table(bt, tableid, hits, 17);
	if (nh > 16) {                     /* the 17th+ index is not maintained */
		xtc_atomic_store_i32(&g_idx_taint, 1, xtc_mo_release);
		nh = 16;
	}
	for (i = 0; i < nh; i++) {
		nkl = okl = -1;
		if (!deleted && newrec != NULL && newlen > 0)
			nkl = xs_idx_rowkey(&hits[i], newrec, newlen, rowid, 1,
			    nk, (int)sizeof nk, NULL);
		if (oldrec != NULL && oldlen > 0)
			okl = xs_idx_rowkey(&hits[i], oldrec, oldlen, rowid, 1,
			    ok, (int)sizeof ok, NULL);
		/* Every row has an entry (long keys are truncated): -1 only
		 * on a page too small for one, and then the index is short. */
		if (!deleted && newrec != NULL && newlen > 0 && nkl < 0)
			xtc_atomic_store_i32(&g_idx_taint, 1, xtc_mo_release);
		if (nkl > 0 && okl == nkl && memcmp(nk, ok, (size_t)nkl) == 0)
			continue;                  /* key tuple unchanged */
		if (nkl > 0)
			(void)xs_idx_put(bt, nk, nkl);
		if (okl > 0)
			xs_idx_dead_add(bt, &hits[i], rowid, ts, ok, okl);
	}
}


/* Forward declarations for the UNIQUE / index helpers defined with the
 * index catalog further down. */
struct xs_inflight;
static int  xs_put_ts(bt_t *bt, uint32_t tableid, int64_t rowid,
    const void *blob, int n, int deleted, uint64_t base, int insert,
    uint64_t *out_ts, uint32_t *ubad, int vis);
static int  xs_uniq_collect(bt_t *bt, const xstore_ctx_t *cx, uint32_t tid1,
    struct xs_idx_rt *u, int cap);
static void xs_uniq_stamp_locked(const xstore_ctx_t *cx, uint32_t tid1,
    const uint8_t *rec1, int len1, int64_t rid1,
    const struct xs_idx_rt *u, int nu, uint64_t ts);
static int  xs_uniq_validate_locked(bt_t *bt, xstore_ctx_t *cx, uint32_t tid1,
    int64_t rid1, const uint8_t *rec1, int len1,
    const struct xs_idx_rt *u, int nu, uint32_t *bad);
static int  xs_idx_get(bt_t *bt, uint32_t idx_id, struct xs_idx_ent *e);
static void xs_uniq_msg(const struct xs_idx_ent *e, char *out, size_t cap);
static void xs_idx_err_msg(bt_t *bt, uint32_t idx, int rc, char *o, size_t n);
static int  xs_uniq_check_stmt(xstore_ctx_t *cx, uint32_t tableid,
    int64_t rowid, const uint8_t *rec, int reclen);

/* Convenience: capture a row's committed pre-image for maintenance.
 * Returns the length written to `buf`, 0 if there is none or it could
 * not be read.  Note that an unread pre-image leaves the old version's
 * entries in place: readers verify every entry against its row, so a
 * stale entry is never an answer, but nothing reaps it either, so the
 * indexes are marked untrusted and the next open rebuilds them. */
static int
xs_idx_preimage(bt_t *bt, uint32_t tableid, int64_t rowid,
    uint8_t *buf, int cap)
{
	int n;
	if (!XS_IDX_ANY()) return 0;  /* no indexes: skip the read entirely */
	n = xs_read_rec(bt, tableid, rowid, buf, cap);
	if (n < 0) {
		(void)xs_rd_count(n);
		xtc_atomic_store_i32(&g_idx_taint, 1, xtc_mo_release);
		return 0;
	}
	return n;
}

/* ---- write-buffer hash maps ------------------------------------- *
 *
 * wmap: (tableid, rowid) -> index of the NEWEST wbuf entry for it, so
 * read-your-writes lookups (wbuf_find) are O(1) instead of a backwards
 * scan of the whole buffer -- a 100k-row load transaction otherwise
 * paid O(n^2).  umap: (unique index, key tuple) hash -> wbuf index of a
 * buffered row carrying it, for the statement-time UNIQUE check against
 * the transaction's own writes.  Both are rebuilt on truncation
 * (savepoint / statement rollback) and cleared with the buffer. */
static uint64_t
xs_mix64(uint64_t x)
{
	x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
	x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
	return x ^ (x >> 33);
}

static uint64_t
xs_bytes_hash(const uint8_t *p, int n)
{
	uint64_t h = 0xcbf29ce484222325ull;
	int i;
	for (i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
	return xs_mix64(h);
}

static void
wmap_put(xstore_ctx_t *c, uint32_t tid, int64_t rid, int wi)
{
	uint64_t h;
	int i, m;
	if (c->wmap_n * 2 >= c->wmap_cap) {
		int nc = c->wmap_cap ? c->wmap_cap * 2 : 64, k;
		struct xs_wmap *nm = malloc(sizeof *nm * (size_t)nc), *om = c->wmap;
		int ocap = c->wmap_cap;
		if (nm == NULL) { c->wmap_bad = 1; return; }
		for (k = 0; k < nc; k++) nm[k].wi = -1;
		c->wmap = nm; c->wmap_cap = nc; c->wmap_n = 0;
		for (k = 0; k < ocap; k++)
			if (om[k].wi >= 0) wmap_put(c, om[k].tid, om[k].rid, om[k].wi);
		free(om);
	}
	m = c->wmap_cap - 1;
	h = xs_mix64(((uint64_t)tid << 40) ^ (uint64_t)rid);
	for (i = (int)(h & (uint64_t)m); ; i = (i + 1) & m) {
		if (c->wmap[i].wi < 0) {
			c->wmap[i].tid = tid; c->wmap[i].rid = rid; c->wmap[i].wi = wi;
			c->wmap_n++;
			return;
		}
		if (c->wmap[i].tid == tid && c->wmap[i].rid == rid) {
			c->wmap[i].wi = wi;
			return;
		}
	}
}

static int
wmap_get(const xstore_ctx_t *c, uint32_t tid, int64_t rid)
{
	uint64_t h;
	int i, m;
	if (c->wmap_cap == 0) return -1;
	m = c->wmap_cap - 1;
	h = xs_mix64(((uint64_t)tid << 40) ^ (uint64_t)rid);
	for (i = (int)(h & (uint64_t)m); c->wmap[i].wi >= 0; i = (i + 1) & m)
		if (c->wmap[i].tid == tid && c->wmap[i].rid == rid)
			return c->wmap[i].wi;
	return -1;
}

static void
umap_put(xstore_ctx_t *c, uint64_t h, uint32_t idx_id, int wi)
{
	int i, m;
	if (c->umap_n * 2 >= c->umap_cap) {
		int nc = c->umap_cap ? c->umap_cap * 2 : 64, k;
		struct xs_umap *nm = malloc(sizeof *nm * (size_t)nc), *om = c->umap;
		int ocap = c->umap_cap;
		if (nm == NULL) { c->umap_bad = 1; return; }
		for (k = 0; k < nc; k++) nm[k].wi = -1;
		c->umap = nm; c->umap_cap = nc; c->umap_n = 0;
		for (k = 0; k < ocap; k++)
			if (om[k].wi >= 0) umap_put(c, om[k].h, om[k].idx_id, om[k].wi);
		free(om);
	}
	m = c->umap_cap - 1;
	for (i = (int)(h & (uint64_t)m); c->umap[i].wi >= 0; i = (i + 1) & m)
		;
	c->umap[i].h = h; c->umap[i].idx_id = idx_id; c->umap[i].wi = wi;
	c->umap_n++;
}

/* Rebuild both maps over wbuf[0, wn) (after a truncation). */
static void xs_umap_index(xstore_ctx_t *c, int wi);
static void
wmaps_rebuild(xstore_ctx_t *c)
{
	int i;
	for (i = 0; i < c->wmap_cap; i++) c->wmap[i].wi = -1;
	for (i = 0; i < c->umap_cap; i++) c->umap[i].wi = -1;
	c->wmap_n = c->umap_n = 0;
	c->wmap_bad = 0;
	c->umap_bad = 0;
	for (i = 0; i < c->wn; i++) {
		wmap_put(c, c->wbuf[i].tableid, c->wbuf[i].rowid, i);
		xs_umap_index(c, i);
	}
}

/*
 * Enter the SQL transaction on first vtab access.  SQLite fires xBegin
 * at the first WRITE, not the first read, so reads before the first
 * write would otherwise miss the transaction (and the read set, and
 * the start snapshot).  xsql_get_autocommit() == 0 means an explicit
 * BEGIN..COMMIT is open; capture the snapshot and open the
 * buffer/read-set on the first access within it.  Autocommit
 * single-statement work leaves in_txn == 0 (immediate write, latest
 * read).
 */
static void
xs_enter_ctx(xstore_ctx_t *cx, struct xsql *db)
{
	/*
	 * Effective autocommit: in native_mode the native driver owns it
	 * (native_autocommit); otherwise SQLite's connection flag governs,
	 * as before.  The rest of the logic is identical -- close a finished
	 * txn when autocommit is back on but we are still in_txn; open a txn
	 * when autocommit is off and we are not yet in_txn.
	 */
	/* The connection is always native: autocommit is engine-owned. */
	int autocommit = cx->native_autocommit;
	(void)db;
	if (cx->in_txn && autocommit) {
		if (cx->ssi != NULL) { ssi_abort(cx->ssi); cx->ssi = NULL; }
		wbuf_clear(cx);
		cx->in_txn = 0;
		xs_pin_recompute(cx);
	}
	if (!autocommit && !cx->in_txn) {
		cx->in_txn = 1;
		cx->txn_snap = snap_pin_visible(&cx->snap_slot);
		wbuf_clear(cx);
		cx->steal_txn = 0;
		cx->rn = 0;
		cx->did_scan = 0;
		cx->txn_iso = cx->isolation;
		cx->touched = 0;
		cx->serializable = (cx->txn_iso == XS_ISO_SERIALIZABLE);
		cx->ssi = cx->serializable ? ssi_begin(cx->txn_snap) : NULL;
	}
}

#if defined(SQLXTC_VTAB)
static void
xs_enter(xstore_vtab_t *v)
{
	xs_enter_ctx(v->ctx, v->db);
}
#endif /* SQLXTC_VTAB */

typedef struct xstore_cursor {
#if defined(SQLXTC_VTAB)
	xsql_vtab_cursor base;
#endif
	bt_t        *bt;
	xstore_ctx_t *ctx;
	uint32_t     tableid;       /* version namespace of the scanned table */
	uint64_t     snap;          /* the read snapshot for this scan */
	int          point;         /* 1 == point lookup (<= one row) */
	int          eof;
	int64_t      lo, hi;         /* range scan bounds (rowid), if has_* */
	int          has_lo, has_hi;
	int          have_last;      /* last_key/last_rowid are valid */
	uint8_t      last_key[XS_VKLEN]; /* resume point (key last consumed) */
	int64_t      last_rowid;     /* rowid already resolved (scan dedup) */
	int64_t      rowid;          /* current visible row */
	uint8_t      val[XS_VMAX];
	uint16_t     vlen;
	bt_cursor_t *btc;            /* persistent scan cursor, parked between calls */
	uint64_t     visited;        /* version keys examined (metric, flushed at close) */
	int          err;            /* XTC_E_* that ended the walk, or 0 */
} xstore_cursor_t;

/*
 * The cursor holds NO btree page latch between calls.  SQLite runs an
 * UPDATE/DELETE by keeping its read cursor open and calling xUpdate
 * between xNext calls; if the cursor held a shared content latch, the
 * xUpdate's bt_insert (exclusive latch on the same page) would
 * self-deadlock on the one thread.  So each xFilter/xNext opens a
 * short-lived btree cursor, copies the row out, and closes it --
 * releasing the latch before control returns to the VDBE.  Resuming by
 * key costs an O(log n) re-descent per row; a latch-releasing,
 * position-revalidating btree cursor would restore O(1) amortized
 * (see bench/sqlxtc/PERF_IDEAS.md).
 */

#if defined(SQLXTC_VTAB)
static const xsql_module xstore_module;   /* fwd */
#endif

/* The record codec (xstore_rec_put / xstore_rec_col) is defined with the
 * scan API below. */

/* Encode `npay` payload values into `out` (capacity `cap`).  Returns the
 * encoded length, or -1 if it would overflow `cap` / 255 columns. */
#if defined(SQLXTC_VTAB)
static int
xs_rec_encode(xsql_value **vals, int npay, uint8_t *out, int cap)
{
	int off = 0, i;
	if (npay < 0 || npay > 255 || cap < 1)
		return -1;
	out[off++] = (uint8_t)npay;
	for (i = 0; i < npay && off >= 0; i++) {
		xsql_value *v = vals[i];
		int t = xsql_value_type(v);
		if (t == SQLITE_NULL)
			off = xstore_rec_put(out, off, cap, XSTORE_C_NULL, 0, 0, NULL, 0);
		else if (t == SQLITE_INTEGER)
			off = xstore_rec_put(out, off, cap, XSTORE_C_INT,
			    xsql_value_int64(v), 0, NULL, 0);
		else if (t == SQLITE_FLOAT)
			off = xstore_rec_put(out, off, cap, XSTORE_C_REAL, 0,
			    xsql_value_double(v), NULL, 0);
		else if (t == SQLITE_TEXT)
			off = xstore_rec_put(out, off, cap, XSTORE_C_TEXT, 0, 0,
			    (const uint8_t *)xsql_value_text(v), xsql_value_bytes(v));
		else
			off = xstore_rec_put(out, off, cap, XSTORE_C_BLOB, 0, 0,
			    xsql_value_blob(v), xsql_value_bytes(v));
	}
	return off;
}
#endif /* SQLXTC_VTAB */

/* Decode the `idx`-th payload value (0-based) from `rec` into `ctx`.
 * Out-of-range or malformed records resolve to NULL. */
#if defined(SQLXTC_VTAB)
static void
xs_rec_result(xsql_context *ctx, const uint8_t *rec, int reclen, int idx)
{
	int64_t iv = 0; double rv = 0; const uint8_t *p = NULL; int n = 0;
	switch (xstore_rec_col(rec, reclen, idx, &iv, &rv, &p, &n)) {
	case XSTORE_C_INT:  xsql_result_int64(ctx, iv); break;
	case XSTORE_C_REAL: xsql_result_double(ctx, rv); break;
	case XSTORE_C_TEXT:
		xsql_result_text(ctx, (const char *)p, n, SQLITE_TRANSIENT);
		break;
	case XSTORE_C_BLOB: xsql_result_blob(ctx, p, n, SQLITE_TRANSIENT); break;
	default:            xsql_result_null(ctx); break;
	}
}
#endif /* SQLXTC_VTAB */

#if defined(SQLXTC_VTAB)
static int
xs_connect(xsql *db, void *pAux, int argc, const char *const *argv,
    xsql_vtab **ppv, char **pzErr)
{
	xstore_vtab_t *v;
	int rc, i, npay;
	char coldefs[1024];
	int coff = 0;
	const char *cdp = NULL;
	(void)pzErr;

	if (argc <= 3) {
		/* No column list: the historical k/v default, exactly. */
		rc = xsql_declare_vtab(db,
		    "CREATE TABLE x(k INTEGER PRIMARY KEY, v)");
		npay = 1;
		coff = snprintf(coldefs, sizeof coldefs, "k,v");
		if (coff > 0 && coff < (int)sizeof coldefs) cdp = coldefs;
	} else {
		/* USING xstore(key, col1, col2, ...): the first arg is the
		 * INTEGER PRIMARY KEY (the btree rowid); the rest are payload
		 * columns encoded into the version record. */
		char decl[1024];
		int off = snprintf(decl, sizeof decl,
		    "CREATE TABLE x(%s INTEGER PRIMARY KEY", argv[3]);
		if (off < 0 || off >= (int)sizeof decl)
			return SX_ERROR;
		coff = snprintf(coldefs, sizeof coldefs, "%s", argv[3]);
		if (coff < 0 || coff >= (int)sizeof coldefs) coff = -1;
		for (i = 4; i < argc; i++) {
			int w = snprintf(decl + off, sizeof decl - (size_t)off,
			    ", %s", argv[i]);
			if (w < 0 || w >= (int)sizeof decl - off)
				return SX_ERROR;   /* declaration too long */
			off += w;
			if (coff >= 0) {
				int cw = snprintf(coldefs + coff, sizeof coldefs - (size_t)coff,
				    ",%s", argv[i]);
				if (cw < 0 || cw >= (int)sizeof coldefs - coff) coff = -1;
				else coff += cw;
			}
		}
		if (off + 2 > (int)sizeof decl)
			return SX_ERROR;
		decl[off++] = ')'; decl[off] = '\0';
		rc = xsql_declare_vtab(db, decl);
		npay = argc - 4;
		if (coff > 0) cdp = coldefs;
	}
	if (rc != SX_OK)
		return rc;
	v = xsql_malloc(sizeof *v);
	if (v == NULL)
		return SX_NOMEM;
	memset(v, 0, sizeof *v);
	v->ctx = (xstore_ctx_t *)pAux;
	v->db = db;
	v->npay = npay;
	/* argv[2] is the table name: map it to a stable per-B-tree id and
	 * persist its column schema (cdp) so the native paths read the
	 * schema from the catalog, not sqlite_master. */
	v->tableid = xs_cat_find_or_create(v->ctx->bt, argv[2], cdp, NULL,
	    NULL);
	if (v->tableid == 0) {
		xsql_free(v);
		return SX_ERROR;
	}
	*ppv = &v->base;
	return SX_OK;
}
#endif /* SQLXTC_VTAB */

#if defined(SQLXTC_VTAB)
static int
xs_disconnect(xsql_vtab *pv)
{
	xsql_free(pv);
	return SX_OK;
}
#endif /* SQLXTC_VTAB */

#if defined(SQLXTC_VTAB)
static int
xs_best_index(xsql_vtab *pv, xsql_index_info *info)
{
	int i, idx = 0, argc = 0, eq = -1, lo = -1, hi = -1;
	(void)pv;
	for (i = 0; i < info->nConstraint; i++) {
		if (!info->aConstraint[i].usable || info->aConstraint[i].iColumn != 0)
			continue;
		switch (info->aConstraint[i].op) {
		case SQLITE_INDEX_CONSTRAINT_EQ: eq = i; break;
		case SQLITE_INDEX_CONSTRAINT_GT:
		case SQLITE_INDEX_CONSTRAINT_GE: lo = i; break;
		case SQLITE_INDEX_CONSTRAINT_LT:
		case SQLITE_INDEX_CONSTRAINT_LE: hi = i; break;
		default: break;
		}
	}
	if (eq >= 0) {                          /* point lookup */
		info->aConstraintUsage[eq].argvIndex = 1;
		info->aConstraintUsage[eq].omit = 1;
		info->idxNum = 1;
		info->estimatedCost = 1.0;
		info->estimatedRows = 1;
		return SX_OK;
	}
	/* Bounded range: pass the bounds to xFilter (idxNum bits 2/4), but
	 * do NOT omit -- the bounds drive the scan seek/stop and the SSI
	 * predicate; SQLite re-applies the exact (possibly exclusive)
	 * comparison, so an inclusive scan superset is safe. */
	if (lo >= 0) { info->aConstraintUsage[lo].argvIndex = ++argc; idx |= 2; }
	if (hi >= 0) { info->aConstraintUsage[hi].argvIndex = ++argc; idx |= 4; }
	info->idxNum = idx;
	info->estimatedCost = idx ? 100.0 : 1.0e6;
	return SX_OK;
}
#endif /* SQLXTC_VTAB */

#if defined(SQLXTC_VTAB)
static int
xs_open(xsql_vtab *pv, xsql_vtab_cursor **ppc)
{
	xstore_vtab_t *v = (xstore_vtab_t *)pv;
	xstore_cursor_t *c = xsql_malloc(sizeof *c);
	if (c == NULL)
		return SX_NOMEM;
	memset(c, 0, sizeof *c);
	c->bt = v->ctx->bt;
	c->ctx = v->ctx;
	c->tableid = v->tableid;
	c->eof = 1;
	*ppc = &c->base;
	return SX_OK;
}
#endif /* SQLXTC_VTAB */

#if defined(SQLXTC_VTAB)
static int
xs_close(xsql_vtab_cursor *pc)
{
	xstore_cursor_t *c = (xstore_cursor_t *)pc;
	if (c->btc != NULL) {           /* release any parked scan cursor */
		bt_cursor_close(c->btc);
		c->btc = NULL;
	}
	xsql_free(pc);          /* no latch/cursor held between calls */
	return SX_OK;
}
#endif /* SQLXTC_VTAB */

/* Copy the value payload (after the flag byte) into the cursor. */
static void
xs_stash(xstore_cursor_t *c, int64_t rowid, const uint8_t *val, uint16_t vl)
{
	c->rowid = rowid;
	c->vlen = (vl > 1) ? (uint16_t)(vl - 1) : 0;
	if (c->vlen > XS_VMAX) c->vlen = XS_VMAX;
	if (c->vlen) memcpy(c->val, val + 1, c->vlen);
}

static int g_scan_failat;   /* test hook: the scan read to fail, 1-based */
/* Test hook (xstore_implicit_commit_hook); NULL in production. */
static void (*g_icommit_hook)(int phase);

void
xstore_implicit_commit_hook(void (*fn)(int phase))
{
	g_icommit_hook = fn;
}

/* Should this cursor read of a base-table scan fail
 * (xstore_scan_read_fail_at)?  One relaxed load when disarmed. */
static int
xs_scan_fault(void)
{
	return xtc_atomic_load_i32(&g_scan_failat, xtc_mo_relaxed) > 0 &&
	    xtc_atomic_fetch_sub_i32(&g_scan_failat, 1, xtc_mo_relaxed) == 1;
}

int
xstore_scan_read_fail_at(int n)
{
	return xtc_atomic_exchange_i32(&g_scan_failat, n, xtc_mo_relaxed);
}

/* Position the scan's B-tree cursor for the next step: open it (one
 * descent to the table's first key, or the lo bound) or resume it past
 * the last key consumed.  0, or -1 at the end (c->eof set); a cursor
 * that could not be opened or resumed also leaves its error in c->err. */
static int
xs_cur_resume(xstore_cursor_t *c)
{
	int rc;

	if (c->btc == NULL) {
		uint8_t startbuf[XS_VKLEN];
		const uint8_t *start;
		if (c->have_last) {
			start = c->last_key;
		} else {
			/* Seek to this table's lowest key (or the lo bound): a
			 * single B-tree holds many tables, so a scan must start
			 * at our table-id and stop when it changes. */
			enc_vkey(c->tableid, c->has_lo ? c->lo : INT64_MIN,
			    ~(uint64_t)0, startbuf);
			start = startbuf;
		}
		rc = bt_cursor_open(c->bt, start, XS_VKLEN, &c->btc);
	} else {
		rc = bt_cursor_resume(c->btc);
	}
	if (rc == XTC_OK && xs_scan_fault())
		rc = XTC_E_IO;
	if (rc != XTC_OK) {
		c->err = rc;
		c->eof = 1;
		return -1;
	}
	return 0;
}

/* One step of the visibility walk on a positioned cursor (latch held):
 * 1 with the next visible row -- its rowid and stored value (flag byte
 * first; valid until the next cursor call) -- or 0 at the end (c->eof
 * set).  A cursor read error also ends the walk, with c->err set: the
 * rows not read are unknown, not absent.  Visible = the newest
 * non-tombstone version of each rowid with commit_ts <= snap
 * (PostgreSQL HeapTupleSatisfiesMVCC). */
static int
xs_cur_step(xstore_cursor_t *c, int64_t *rid_out, const uint8_t **val,
    uint16_t *vlen)
{
	/* The last row resolved is the range's upper bound: done, without
	 * walking that row's older versions (a hot row updated N times since
	 * the last GC has N of them; a rowid point read paid all N). */
	if (c->have_last && c->has_hi && c->last_rowid >= c->hi) {
		c->eof = 1;
		return 0;
	}
	for (;;) {
		const void *k = NULL, *vv = NULL;
		uint16_t klen = 0, vl = 0;
		const uint8_t *kb, *vb;
		int64_t rid;
		uint64_t ts;
		int rc;

		rc = bt_cursor_next(c->btc, &k, &klen, &vv, &vl);
		if (rc == XTC_OK && xs_scan_fault())
			rc = XTC_E_IO;
		if (rc != XTC_OK && rc != XTC_E_NOTFOUND)
			c->err = rc;
		if (rc != XTC_OK || klen != XS_VKLEN) {
			c->eof = 1;
			return 0;
		}
		c->visited++;
		kb = (const uint8_t *)k; vb = (const uint8_t *)vv;
		if (dec_tableid(kb) != c->tableid) {
			c->eof = 1;             /* ran past our table into the next */
			return 0;
		}
		rid = dec_rowid(kb);
		ts = dec_ts(kb);
		if (c->has_hi && rid > c->hi) {
			c->eof = 1;             /* past the upper bound: range exhausted */
			return 0;
		}
		if (c->have_last && rid == c->last_rowid)
			continue;              /* the resume key, or an older version */
		if (ts > c->snap)
			continue;              /* created after the snapshot */
		c->last_rowid = rid;
		c->have_last = 1;
		memcpy(c->last_key, kb, XS_VKLEN);
		if (vl >= 1 && (vb[0] & XS_F_DELETED))
			continue;              /* deleted at the snapshot */
		*rid_out = rid;
		*val = vb;
		*vlen = vl;
		c->eof = 0;
		return 1;
	}
}

/* Advance a scan to the next visible row (into c->rowid / c->val).  The
 * scan cursor is persistent: opened once with a full descent, then
 * resumed in O(1) past the last key consumed.  The leaf latch is held
 * only for the duration of this call -- the cursor is PARKED (latch
 * released) before control returns to the caller, so an interleaved
 * write cannot self-deadlock against a held content latch. */
static void
xs_advance(xstore_cursor_t *c)
{
	int64_t rid;
	const uint8_t *vb;
	uint16_t vl;

	if (c->have_last && c->has_hi && c->last_rowid >= c->hi) {
		c->eof = 1;
		return;
	}
	/* Both set c->err and c->eof on a read error; every caller of
	 * xs_advance tests c->err. */
	if (xs_cur_resume(c) != 0)   /* RDERR_OK: c->err */
		return;
	if (xs_cur_step(c, &rid, &vb, &vl))   /* RDERR_OK: c->err */
		xs_stash(c, rid, vb, vl);
	bt_cursor_park(c->btc);            /* release the latch before returning */
}

/* ---- storage-native scan (public; see xstore.h) ------------------ *
 *
 * Drives the same xs_advance visibility loop as the vtab cursor, but
 * standalone: no SQLite connection, no vtab base, no txn context.  The
 * executor opens one of these per worker over a disjoint rowid range. */
static int xs_row_live(bt_t *bt, uint32_t tableid, int64_t rowid);
static int ciprefix(const char *s, const char *pfx);
static int xs_rollback_ctx(xstore_ctx_t *cx);

struct xstore_scan {
	xstore_cursor_t c;
	int             done;      /* sticky end-of-scan */
	/* Read-your-writes merge (txn scans only): when ryw_ctx != NULL the
	 * scan overlays the connection's uncommitted writes -- a buffered
	 * UPDATE replaces the committed row, a buffered DELETE hides it, and
	 * after the committed range is drained, buffered INSERTs (rowids not
	 * in the committed tree) are emitted in ascending rowid order. */
	xstore_ctx_t   *ryw_ctx;   /* connection write buffer, or NULL */
	int             ryw_overlay; /* 1 = overlay the write buffer (wn > 0) */
	int             ryw_track;   /* 1 = record reads into the SSI read set */
	int             ryw_phase; /* 0 = committed range, 1 = wbuf-only inserts */
	/* The rowids of this txn's live buffered rows in range, ascending:
	 * merged into the committed walk so a buffered INSERT into a rowid
	 * hole is returned in rowid order (it was missed when it sorted below
	 * the highest committed rowid). */
	int64_t        *ryw_ins;
	int             ryw_nins, ryw_ii;
	int             ryw_held;        /* s->c holds a committed row not yet returned */
	uint8_t         ryw_val[XS_VMAX]; /* staging for a wbuf payload */
	/* Read-ahead (plain committed scans): visible rows copied out of the
	 * tree in batches, each from ONE cursor resume, instead of one resume
	 * (pin, shared latch, leaf binary search) and park per row.  The
	 * first row is read alone (a point or LIMIT 1 read never fills a
	 * batch); then batches grow 16, 32, .. XS_RA_MAX rows. */
	struct xs_ra   *ra;
	int             ra_n, ra_i, ra_want;
	xstore_ctx_t   *err_cx;    /* connection a read error is reported to */
	char            name[64];  /* the table, for that report */
};

/* A read-ahead batch: up to XS_RA_MAX rows, their payloads packed in
 * buf (a full batch always has room for one more XS_VMAX payload). */
#define XS_RA_MAX   128
#define XS_RA_BYTES (16 * 1024)
struct xs_ra {
	int64_t  rowid[XS_RA_MAX];
	uint32_t off[XS_RA_MAX];
	uint16_t len[XS_RA_MAX];
	uint8_t  buf[XS_RA_BYTES + XS_VMAX];
};

static int
xs_i64_cmp(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
	return (x > y) - (x < y);
}

/* xstore_scan_open, saying why it returned NULL: *err is XTC_E_NOTFOUND
 * (no such table), XTC_E_NOMEM, or the catalog read's error. */
static xstore_scan_t *
xs_scan_new(bt_t *bt, const char *table, uint64_t snap, int64_t lo,
    int has_lo, int64_t hi, int has_hi, int *err)
{
	xstore_scan_t *s;
	uint32_t tid;
	int r;
	*err = XTC_E_NOTFOUND;
	if (bt == NULL || table == NULL) return NULL;
	if ((r = xstore_table_id(bt, table, &tid)) <= 0) {
		if (r < 0) *err = r;               /* unread, not unknown */
		return NULL;
	}
	*err = XTC_E_NOMEM;
	s = (xstore_scan_t *)calloc(1, sizeof *s);
	if (s == NULL) return NULL;
	*err = XTC_OK;
	s->c.bt = bt;
	s->c.tableid = tid;
	s->c.snap = snap ? snap : xs_visible();
	s->c.lo = lo; s->c.has_lo = has_lo ? 1 : 0;
	s->c.hi = hi; s->c.has_hi = has_hi ? 1 : 0;
	s->c.have_last = 0;
	s->c.eof = 0;
	s->c.btc = NULL;
	return s;
}

xstore_scan_t *
xstore_scan_open(bt_t *bt, const char *table, uint64_t snap,
                 int64_t lo, int has_lo, int64_t hi, int has_hi)
{
	int err;
	return xs_scan_new(bt, table, snap, lo, has_lo, hi, has_hi, &err);
}

/*
 * The highest rowid of `table` whose version visible at the
 * connection's read snapshot is live (not a tombstone), within an
 * optional upper bound hi (inclusive).  Walks rowids DOWNWARD with
 * bt_seek_lt -- one descent per rowid -- and stops at the first live
 * one, so MAX(rowid) is O(log n) instead of a scan of the whole table
 * (403 ms at 2M rows; the ledger "recent N" slice runs it per query).
 * A rowid whose newest visible version is a tombstone, or that has only
 * versions newer than the snapshot, is skipped.  Inside a transaction
 * the connection's buffered writes are merged: a buffered row above the
 * committed answer wins, a buffered DELETE hides a committed row.
 * `snap` 0 = the connection's read snapshot (as xstore_scan_open_txn).
 * Returns 1 with *out, 0 if the table has no live row (<= hi), or -1
 * when a read failed (rd_err says which table): the answer is unknown.
 */
static int xs_point_get(bt_t *bt, uint32_t tid, int64_t rid, uint64_t snap,
    uint8_t *out, int cap, int *err);
int
xstore_max_live_rowid(struct xsql *db, const char *table, uint64_t snap,
    int64_t hi, int has_hi, int64_t *out)
{
	bt_t *bt = xstore_bt_of(db);
	xstore_ctx_t *cx = xstore_ctx_of(db);
	uint8_t bound[XS_VKLEN], k[XS_VKLEN + 8];
	uint32_t tid;
	int64_t bmax = 0, rid;
	int have_b = 0, guard, i, rc, err = 0;

	if (bt == NULL || table == NULL)
		return 0;
	if ((rc = xstore_table_id(bt, table, &tid)) <= 0) {
		if (rc == 0) return 0;           /* unknown table */
		xs_rd_err_tid(cx, XS_CAT_TABLEID, rc);   /* counted by the read */
		return -1;
	}
	if (cx != NULL) {
		uint64_t rs =
		    xtc_atomic_load_u64(&cx->read_snap, xtc_mo_relaxed);
		/* the read snapshot, chosen exactly as xstore_scan_open_txn does */
		if (snap != 0) { /* caller's */ }
		else if (rs != 0) snap = rs;
		else if (cx->in_txn && cx->txn_iso >= XS_ISO_REPEATABLE_READ) snap = cx->txn_snap;
		else if (cx->stmt_base != 0) snap = cx->stmt_base;
		if (cx->in_txn) {
			cx->touched = 1;
			for (i = 0; i < cx->wn; i++) {
				const xs_wrec_t *w = &cx->wbuf[i];
				if (w->tableid != tid || w->deleted || !wbuf_is_newest(cx, i) ||
				    (has_hi && w->rowid > hi))
					continue;
				if (!have_b || w->rowid > bmax) { bmax = w->rowid; have_b = 1; }
			}
		}
	}
	if (snap == 0) snap = xs_visible();
	/* Exclusive upper bound: just above the bound rowid (any version). */
	/* Exclusive bound: the smallest key of rowid hi + 1 (its newest
	 * version sorts first), or the first key of the next table. */
	if (has_hi && hi < INT64_MAX)
		enc_vkey(tid, hi + 1, ~(uint64_t)0, bound);
	else if (tid < UINT32_MAX)
		enc_vkey(tid + 1, INT64_MIN, ~(uint64_t)0, bound);
	else
		return 0;
	for (guard = 0; guard < (1 << 20); guard++) {
		uint16_t klen = 0;
		uint8_t val[1 + XS_VMAX];
		const xs_wrec_t *w;
		int live;
		rc = bt_seek_lt(bt, bound, XS_VKLEN, k, sizeof k, &klen, NULL, 0,
		    NULL);
		if (rc == XTC_OK && xs_row_fault())
			rc = XTC_E_IO;
		if (rc != XTC_OK && rc != XTC_E_NOTFOUND) {
			err = rc;
			break;
		}
		if (rc != XTC_OK || klen != XS_VKLEN || dec_tableid(k) != tid)
			break;                       /* no lower rowid in this table */
		rid = dec_rowid(k);
		if (have_b && bmax >= rid) {     /* a buffered row is the answer */
			*out = bmax;
			return 1;
		}
		/* The row's version visible at snap (newest <= snap). */
		live = 0;
		w = (cx != NULL && cx->in_txn) ? wbuf_find(cx, tid, rid) : NULL;
		if (w != NULL)
			live = !w->deleted;
		else
			live = xs_point_get(bt, tid, rid, snap, val,
			           (int)sizeof val, &err) != 0;
		if (err == 0 && w == NULL && xs_row_fault())
			err = XTC_E_IO;
		if (err != 0)
			break;
		if (live) {
			*out = rid;
			return 1;
		}
		enc_vkey(tid, rid, ~(uint64_t)0, bound);   /* below every version */
	}
	if (err != 0) {
		(void)xs_rd_count(err);
		xs_rd_err_tid(cx, tid, err);
		return -1;
	}
	if (have_b) { *out = bmax; return 1; }
	return 0;
}

/*
 * Open a scan that honours the connection's uncommitted writes
 * (read-your-writes).  Identical to xstore_scan_open when the connection
 * is not in a transaction or its write buffer is empty; otherwise the
 * returned scan overlays the buffer (UPDATE replaces, DELETE hides,
 * INSERT appended after the committed range).  `db` is the native
 * connection; the committed snapshot is the txn snapshot.
 */
xstore_scan_t *
xstore_scan_open_txn(struct xsql *db, const char *table, uint64_t snap,
                     int64_t lo, int has_lo, int64_t hi, int has_hi)
{
	bt_t *bt = xstore_bt_of(db);
	xstore_ctx_t *ctx = xstore_ctx_of(db);
	xstore_scan_t *s;
	int err;
	/* Effective read snapshot, mirroring the vtab cursor (xs_filter): an
	 * explicit ctx->read_snap (AS OF), else the txn's frozen snapshot for
	 * REPEATABLE READ and stricter, else the latest committed clock for
	 * READ COMMITTED / autocommit.  A caller-supplied snap overrides. */
	if (snap == 0 && ctx != NULL) {
		uint64_t rs =
		    xtc_atomic_load_u64(&ctx->read_snap, xtc_mo_relaxed);
		if (rs != 0)
			snap = rs;
		else if (ctx->in_txn && ctx->txn_iso >= XS_ISO_REPEATABLE_READ)
			snap = ctx->txn_snap;
		else if (ctx->stmt_base != 0)
			snap = ctx->stmt_base;   /* one snapshot for a DML statement */
		/* else leave 0 -> xstore_scan_open uses the latest clock */
	}
	s = xs_scan_new(bt, table, snap, lo, has_lo, hi, has_hi, &err);
	if (s == NULL) {
		if (err != XTC_E_NOTFOUND && err != XTC_E_NOMEM)
			xs_rd_err_tid(ctx, XS_CAT_TABLEID, err);
		return NULL;
	}
	s->err_cx = ctx;
	snprintf(s->name, sizeof s->name, "%s", table);
	/* Inside a txn, attach the connection ctx so the scan can (a) overlay
	 * the write buffer for read-your-writes (when it has buffered writes)
	 * and (b) record each read into the SSI read set (when serializable),
	 * exactly as the vtab cursor does.  Outside a txn the plain committed
	 * scan is exact. */
	if (ctx != NULL && ctx->in_txn) {
		ctx->touched = 1;
		s->ryw_ctx = ctx;
		s->ryw_overlay = (ctx->wn > 0);
		s->ryw_track = (ctx->serializable != 0);
		s->ryw_phase = 0;
		if (s->ryw_overlay) {
			int i, cap = 0;
			for (i = 0; i < ctx->wn; i++) {
				const xs_wrec_t *w = &ctx->wbuf[i];
				if (w->tableid != s->c.tableid || w->deleted ||
				    (s->c.has_lo && w->rowid < s->c.lo) ||
				    (s->c.has_hi && w->rowid > s->c.hi) ||
				    !wbuf_is_newest(ctx, i))
					continue;
				if (s->ryw_nins == cap) {
					int64_t *nb;
					cap = cap ? cap * 2 : 16;
					nb = realloc(s->ryw_ins, sizeof *nb * (size_t)cap);
					if (nb == NULL) { xstore_scan_close(s); return NULL; }
					s->ryw_ins = nb;
				}
				s->ryw_ins[s->ryw_nins++] = w->rowid;
			}
			if (s->ryw_nins > 1)
				qsort(s->ryw_ins, (size_t)s->ryw_nins, sizeof *s->ryw_ins,
				    xs_i64_cmp);
		}
	}
	return s;
}

/* Copy buffered write `w`'s payload into the scan's staging buffer,
 * first bringing it back from the STEAL staging area if it was spilled.
 * 0, or -1 with s->c.err set when the spilled payload cannot be read:
 * the row is unknown, so the scan must fail rather than return it. */
static int
xs_scan_load_buffered(xstore_scan_t *s, const xs_wrec_t *w)
{
	int rc;
	if (w->spilled && xs_scan_fault())
		rc = SX_ERROR;
	else
		rc = xs_wrec_ensure(s->ryw_ctx, (xs_wrec_t *)w);
	if (rc != SX_OK) {
		s->c.err = rc == SX_NOMEM ? XTC_E_NOMEM : XTC_E_IO;
		return -1;
	}
	if (w->len && w->data) memcpy(s->ryw_val, w->data, w->len);
	return 0;
}

/* Return buffered row `rowid` (its newest write) through the scan outs;
 * 0 if that write is a delete, -1 if its payload cannot be read. */
static int
xs_scan_emit_buffered(xstore_scan_t *s, int64_t rid, int64_t *rowid,
    const uint8_t **rec, int *reclen)
{
	const xs_wrec_t *w = wbuf_find(s->ryw_ctx, s->c.tableid, rid);
	if (w == NULL || w->deleted) return 0;
	if (xs_scan_load_buffered(s, w) != 0) return -1;
	if (rowid) *rowid = rid;
	if (rec) *rec = s->ryw_val;
	if (reclen) *reclen = (int)w->len;
	return 1;
}

/* Refill the read-ahead batch from one cursor resume: up to ra_want
 * visible rows.  Returns the row count (0 at the end of the scan), or
 * -1 when the batch cannot be allocated. */
static int
xs_scan_fill(xstore_scan_t *s)
{
	xstore_cursor_t *c = &s->c;
	uint32_t used = 0;
	int64_t rid;
	const uint8_t *vb;
	uint16_t vl, n;

	s->ra_n = s->ra_i = 0;
	if (c->have_last && c->has_hi && c->last_rowid >= c->hi) {
		c->eof = 1;                    /* a point range ends unallocated */
		return 0;
	}
	if (s->ra == NULL && (s->ra = malloc(sizeof *s->ra)) == NULL)
		return -1;
	/* A read error is in c->err; 0 ends the scan in xs_scan_end, which
	 * reports it. */
	if (xs_cur_resume(c) != 0)   /* RDERR_OK: c->err */
		return 0;
	while (s->ra_n < s->ra_want && used <= XS_RA_BYTES &&
	    xs_cur_step(c, &rid, &vb, &vl)) {   /* RDERR_OK: c->err */
		n = vl > 1 ? (uint16_t)(vl - 1) : 0;
		if (n > XS_VMAX) n = XS_VMAX;
		if (n) memcpy(s->ra->buf + used, vb + 1, n);
		s->ra->rowid[s->ra_n] = rid;
		s->ra->off[s->ra_n] = used;
		s->ra->len[s->ra_n] = n;
		used += n;
		s->ra_n++;
	}
	bt_cursor_park(c->btc);            /* release the latch before returning */
	if (s->ra_want < XS_RA_MAX) s->ra_want *= 2;
	return s->ra_n;
}

/* A plain committed scan of ONE rowid (lo == hi): the version visible
 * at the snapshot is one first-key->= seek (xs_point_get), no cursor.
 * xs_advance would open a B-tree cursor (an allocation, a latched
 * descent, a close) and walk the row's newer versions.  Kept out of
 * line: its value buffer is a page, and the per-row scan path should
 * not carry that frame.  Fills s->c (rowid, val); 0 at none, or when
 * the row could not be read, with s->c.err set. */
#if defined(__GNUC__) || defined(__clang__)
#define XS_NOINLINE __attribute__((noinline))
#else
#define XS_NOINLINE
#endif
static int XS_NOINLINE
xs_scan_point(xstore_scan_t *s)
{
	uint8_t v[1 + XS_VMAX];
	int n;

	s->c.visited++;
	/* The error is in s->c.err; xs_scan_end reports it. */
	n = xs_point_get(s->c.bt, s->c.tableid, /* RDERR_OK: c.err */
	    s->c.lo, s->c.snap, v,
	    (int)sizeof v, &s->c.err);
	if (s->c.err == 0 && xs_scan_fault())
		s->c.err = XTC_E_IO;
	if (n == 0 || s->c.err != 0)
		return 0;
	xs_stash(&s->c, s->c.lo, v, (uint16_t)(n < 0 ? 1 : n + 1));
	return 1;
}

/* End scan `s`: 0 at the true end, or -1 (on this call and every later
 * one) if a read failed, so a caller never takes a short walk for the
 * whole table. */
static int
xs_scan_end(xstore_scan_t *s)
{
	s->done = 1;
	if (s->c.err == 0 || XTC_DST_BUG(XTC_DST_BUG_SCANSHORT))
		return 0;   /* planted 82: a failed read is the table's end */
	(void)xs_rd_count(s->c.err);
	/* The statement's first read error is the one the client sees. */
	if (s->err_cx != NULL && s->err_cx->rd_err[0] == '\0' &&
	    s->err_cx->rd_rc == 0)
		snprintf(s->err_cx->rd_err, sizeof s->err_cx->rd_err,
		    "cannot read table %s: %s", s->name, xtc_strerror(s->c.err));
	return -1;
}

int
xstore_scan_next(xstore_scan_t *s, int64_t *rowid,
                 const uint8_t **rec, int *reclen)
{
	int r;

	if (s == NULL) return -1;
	if (s->done) return xs_scan_end(s);

	/* Plain committed scan: no txn, or a txn that needs neither the
	 * write-buffer overlay nor SSI read tracking. */
	if (s->ryw_ctx == NULL || (!s->ryw_overlay && !s->ryw_track)) {
		if (s->ra_i < s->ra_n) {
			int i = s->ra_i++;
			if (rowid) *rowid = s->ra->rowid[i];
			if (rec) *rec = s->ra->buf + s->ra->off[i];
			if (reclen) *reclen = (int)s->ra->len[i];
			return 1;
		}
		if (s->ra_want == 0 && s->c.has_lo && s->c.has_hi &&
		    s->c.lo == s->c.hi) {             /* a point: one seek */
			s->done = 1;                     /* the only row */
			if (!xs_scan_point(s)) return xs_scan_end(s);
			if (rowid) *rowid = s->c.rowid;
			if (rec) *rec = s->c.val;
			if (reclen) *reclen = (int)s->c.vlen;
			return 1;
		}
		if (s->ra_want == 0) {               /* the first row: alone */
			s->ra_want = 16;
			s->c.eof = 0;
			xs_advance(&s->c);
			if (s->c.eof) return xs_scan_end(s);
			if (rowid) *rowid = s->c.rowid;
			if (rec) *rec = s->c.val;
			if (reclen) *reclen = (int)s->c.vlen;
			return 1;
		}
		if (s->c.eof) return xs_scan_end(s);
		switch (xs_scan_fill(s)) {
		case -1:                              /* no memory for a batch */
			s->c.err = XTC_E_NOMEM;
			return xs_scan_end(s);
		case 0: return xs_scan_end(s);
		default: break;
		}
		s->ra_i = 1;
		if (rowid) *rowid = s->ra->rowid[0];
		if (rec) *rec = s->ra->buf + s->ra->off[0];
		if (reclen) *reclen = (int)s->ra->len[0];
		return 1;
	}

	/* Phase 0: walk the committed range, overlaying the write buffer and
	 * recording reads into the SSI read set; a buffered row with no
	 * committed version that sorts before the next committed row is
	 * returned first, so the merged stream stays in rowid order. */
	if (s->ryw_phase == 0) {
		for (;;) {
			const xs_wrec_t *w;
			if (!s->ryw_held) {
				s->c.eof = 0;
				xs_advance(&s->c);
				if (s->c.err != 0) return xs_scan_end(s);
				if (s->c.eof) { s->ryw_phase = 1; break; }
				s->ryw_held = 1;
				if (s->ryw_track) {
					rset_add(s->ryw_ctx, s->c.tableid, s->c.rowid);
					ssi_record_read(s->ryw_ctx->ssi, s->c.tableid, s->c.rowid);
				}
			}
			if (s->ryw_ii < s->ryw_nins && s->ryw_ins[s->ryw_ii] < s->c.rowid) {
				r = xs_scan_emit_buffered(s, s->ryw_ins[s->ryw_ii++], rowid,
				    rec, reclen);
				if (r < 0) return xs_scan_end(s);
				if (r > 0) return 1;
				continue;
			}
			while (s->ryw_ii < s->ryw_nins && s->ryw_ins[s->ryw_ii] == s->c.rowid)
				s->ryw_ii++;                 /* overlaid below */
			s->ryw_held = 0;
			w = s->ryw_overlay
			    ? wbuf_find(s->ryw_ctx, s->c.tableid, s->c.rowid) : NULL;
			if (w == NULL) {                 /* unbuffered: committed row */
				if (rowid) *rowid = s->c.rowid;
				if (rec) *rec = s->c.val;
				if (reclen) *reclen = (int)s->c.vlen;
				return 1;
			}
			if (w->deleted) continue;        /* buffered DELETE: hide it */
			/* buffered UPDATE: return the buffered payload. */
			if (xs_scan_load_buffered(s, w) != 0) return xs_scan_end(s);
			if (rowid) *rowid = s->c.rowid;
			if (rec) *rec = s->ryw_val;
			if (reclen) *reclen = (int)w->len;
			return 1;
		}
	}

	/* Phase 1: the buffered rows past the last committed row, in order. */
	while (s->ryw_ii < s->ryw_nins) {
		r = xs_scan_emit_buffered(s, s->ryw_ins[s->ryw_ii++], rowid, rec,
		    reclen);
		if (r < 0) return xs_scan_end(s);
		if (r > 0) return 1;
	}
	return xs_scan_end(s);
}

/* Base-table version keys examined by storage scans (the rows-visited
 * metric: a point read touches O(1), a full scan O(table)).  Each scan
 * counts locally and publishes once at close. */
static uint64_t g_rows_visited;

uint64_t
xstore_rows_visited(void)
{
	return xtc_atomic_load_u64(&g_rows_visited, xtc_mo_relaxed);
}

void
xstore_scan_close(xstore_scan_t *s)
{
	if (s == NULL) return;
	if (s->c.visited)
		xtc_atomic_fetch_add_u64(&g_rows_visited, s->c.visited,
		    xtc_mo_relaxed);
	if (s->c.btc != NULL) bt_cursor_close(s->c.btc);
	free(s->ryw_ins);
	free(s->ra);
	free(s);
}

/*
 * Record codec.  A version's payload is the non-key columns of one row:
 *
 *   [ncol:u8] then per column [tag:u8][body]
 *
 * The tag says the storage class and the body's length:
 *
 *   0x00       NULL                          no body
 *   0x01       INTEGER, 8 bytes LE           (1.59 format; still read)
 *   0x02       REAL, 8 bytes LE IEEE-754     (still written for a REAL
 *                                             that is not an integer)
 *   0x03/0x04  TEXT/BLOB, u32 LE length      (1.59 format; still read)
 *   0x05/0x06  TEXT/BLOB, u16 LE length + bytes (64..65535 bytes)
 *   0x08+n     INTEGER, n = 1..7 bytes LE, sign-extended
 *   0x10+n     REAL that is an exact integer: n = 0..7 bytes LE,
 *              sign-extended (n = 0 is 0.0; -0.0 is kept as 0x02)
 *   0x20+n     TEXT of n = 0..63 bytes
 *   0x60+n     BLOB of n = 0..31 bytes
 *   0x80+v     INTEGER v + 64 for v in -64..63 (no body)
 *
 * Every 1.59 record (tags 0-4 only) decodes exactly as before, so a page
 * file or log written by 1.59 is read correctly and nothing needs
 * migrating; a 1.59 binary reading a newer record would see an unknown
 * tag and decode it as NULL, which is why the example does not support
 * downgrades.  This is SQLite's idea (serial types, sqlite3VdbeSerialType)
 * with the length folded into one byte: TPC-C's typical row of small
 * integers and short strings is 40-50% smaller than with 9-byte integers
 * and 5-byte string headers.  The btree value is [flag:u8][record]
 * (xs_put prepends the tombstone flag).
 */

/* Bytes the integer needs, sign-extended (1..8). */
static int
xs_int_width(int64_t v)
{
	uint64_t u = v < 0 ? ~(uint64_t)v : (uint64_t)v;
	int n = 1;

	while (n < 8 && (u >> (8 * n - 1)) != 0)
		n++;
	return n;
}

int
xstore_rec_put(uint8_t *out, int off, int cap, int cls, int64_t iv,
    double rv, const uint8_t *p, int n)
{
	int w, b;
	uint64_t u;

	if (off < 0 || off >= cap)
		return -1;
	switch (cls) {
	case XSTORE_C_NULL:
		out[off++] = 0x00;
		return off;
	case XSTORE_C_INT:
		if (iv >= -64 && iv <= 63) {
			out[off++] = (uint8_t)(0x80 + (iv + 64));
			return off;
		}
		w = xs_int_width(iv);
		if (off + 1 + w > cap)
			return -1;
		out[off++] = (uint8_t)(w == 8 ? 0x01 : 0x08 + w);
		u = (uint64_t)iv;
		for (b = 0; b < w; b++)
			out[off++] = (uint8_t)(u >> (8 * b));
		return off;
	case XSTORE_C_REAL:
		/* An integral REAL in int64 range (not -0.0) keeps its value
		 * exactly in a short integer body; the tag keeps it REAL. */
		if (rv >= -9007199254740992.0 && rv <= 9007199254740992.0 &&
		    rv == (double)(int64_t)rv && !(rv == 0.0 && signbit(rv))) {
			iv = (int64_t)rv;
			w = iv == 0 ? 0 : xs_int_width(iv);
			if (w < 8) {
				if (off + 1 + w > cap)
					return -1;
				out[off++] = (uint8_t)(0x10 + w);
				u = (uint64_t)iv;
				for (b = 0; b < w; b++)
					out[off++] = (uint8_t)(u >> (8 * b));
				return off;
			}
		}
		if (off + 9 > cap)
			return -1;
		out[off++] = 0x02;
		memcpy(&u, &rv, 8);
		for (b = 0; b < 8; b++)
			out[off++] = (uint8_t)(u >> (8 * b));
		return off;
	case XSTORE_C_TEXT:
	case XSTORE_C_BLOB:
		if (n < 0)
			n = 0;
		if (cls == XSTORE_C_TEXT && n <= 63) {
			if (off + 1 + n > cap)
				return -1;
			out[off++] = (uint8_t)(0x20 + n);
		} else if (cls == XSTORE_C_BLOB && n <= 31) {
			if (off + 1 + n > cap)
				return -1;
			out[off++] = (uint8_t)(0x60 + n);
		} else if (n <= 0xFFFF) {
			if (off + 3 + n > cap)
				return -1;
			out[off++] = cls == XSTORE_C_TEXT ? 0x05 : 0x06;
			out[off++] = (uint8_t)n;
			out[off++] = (uint8_t)(n >> 8);
		} else {
			if (off + 5 + n > cap)
				return -1;
			out[off++] = cls == XSTORE_C_TEXT ? 0x03 : 0x04;
			for (b = 0; b < 4; b++)
				out[off++] = (uint8_t)((uint32_t)n >> (8 * b));
		}
		if (n > 0)
			memcpy(out + off, p, (size_t)n);
		return off + n;
	default:
		return -1;
	}
}

/*
 * Decode the column at `off` (its tag byte) of `rec` (length reclen).
 * Fills the class and the value like xstore_rec_col and returns the
 * offset just past the column, or -1 if the record is malformed or
 * truncated there.
 */
static int
xs_rec_get(const uint8_t *rec, int reclen, int off, int *cls, int64_t *iv,
    double *rv, const uint8_t **p, int *n)
{
	uint8_t t;
	uint64_t u = 0;
	uint32_t len;
	int w, b;

	if (off >= reclen)
		return -1;
	t = rec[off++];
	if (t >= 0x80) {
		*cls = XSTORE_C_INT;
		*iv = (int64_t)t - 0x80 - 64;
		return off;
	}
	if (t >= 0x20) {
		len = t >= 0x60 ? (uint32_t)(t - 0x60) : (uint32_t)(t - 0x20);
		if (off + (int)len > reclen)
			return -1;
		*cls = t >= 0x60 ? XSTORE_C_BLOB : XSTORE_C_TEXT;
		*p = rec + off;
		*n = (int)len;
		return off + (int)len;
	}
	switch (t) {
	case 0x00:
		*cls = XSTORE_C_NULL;
		return off;
	case 0x01:
	case 0x02:
		if (off + 8 > reclen)
			return -1;
		for (b = 0; b < 8; b++)
			u |= (uint64_t)rec[off + b] << (8 * b);
		if (t == 0x01) {
			*cls = XSTORE_C_INT;
			*iv = (int64_t)u;
		} else {
			*cls = XSTORE_C_REAL;
			memcpy(rv, &u, 8);
		}
		return off + 8;
	case 0x03: case 0x04: case 0x05: case 0x06:
		w = t <= 0x04 ? 4 : 2;
		if (off + w > reclen)
			return -1;
		for (len = 0, b = 0; b < w; b++)
			len |= (uint32_t)rec[off + b] << (8 * b);
		off += w;
		if (len > (uint32_t)(reclen - off))
			return -1;
		*cls = (t == 0x03 || t == 0x05) ? XSTORE_C_TEXT : XSTORE_C_BLOB;
		*p = rec + off;
		*n = (int)len;
		return off + (int)len;
	default:
		break;
	}
	if (t >= 0x09 && t <= 0x0F) {
		w = t - 0x08;
	} else if (t >= 0x10 && t <= 0x17) {
		w = t - 0x10;
	} else {
		return -1;                     /* not a tag this codec writes */
	}
	if (off + w > reclen)
		return -1;
	for (b = 0; b < w; b++)
		u |= (uint64_t)rec[off + b] << (8 * b);
	if (w > 0 && w < 8 && (u >> (8 * w - 1)) & 1u)
		u |= ~(uint64_t)0 << (8 * w);   /* sign-extend */
	if (t < 0x10) {
		*cls = XSTORE_C_INT;
		*iv = (int64_t)u;
	} else {
		*cls = XSTORE_C_REAL;
		*rv = (double)(int64_t)u;
	}
	return off + w;
}

/* Decode payload column `idx` (0-based, i.e. the non-key columns) of a
 * record produced by the scan into a typed value.  Returns the type
 * (one of XSTORE_C_*); for TEXT/BLOB the p / n outs point into rec (valid for
 * rec's lifetime).  An out-of-range idx, or a malformed record, reads as
 * NULL. */
int
xstore_rec_col(const uint8_t *rec, int reclen, int idx,
               int64_t *iout, double *rout, const uint8_t **pout, int *nout)
{
	int off = 1, i, ncol, cls = XSTORE_C_NULL, n = 0;
	int64_t iv = 0;
	double rv = 0;
	const uint8_t *p = NULL;

	if (rec == NULL || reclen < 1 || idx < 0) return XSTORE_C_NULL;
	ncol = rec[0];
	if (idx >= ncol) return XSTORE_C_NULL;
	for (i = 0; i <= idx; i++)
		if ((off = xs_rec_get(rec, reclen, off, &cls, &iv, &rv, &p, &n)) < 0)
			return XSTORE_C_NULL;
	switch (cls) {
	case XSTORE_C_INT:  if (iout) *iout = iv; break;
	case XSTORE_C_REAL: if (rout) *rout = rv; break;
	case XSTORE_C_TEXT:
	case XSTORE_C_BLOB: if (pout) *pout = p; if (nout) *nout = n; break;
	default: break;
	}
	return cls;
}

int
xstore_rec_cols(const uint8_t *rec, int reclen, xstore_val_t *out, int cap)
{
	int off = 1, i, ncol;

	if (rec == NULL || reclen < 1) return 0;
	ncol = rec[0];
	if (ncol > cap) ncol = cap;
	for (i = 0; i < ncol; i++) {
		out[i].i = 0; out[i].r = 0; out[i].p = NULL; out[i].n = 0;
		if ((off = xs_rec_get(rec, reclen, off, &out[i].cls, &out[i].i,
		    &out[i].r, &out[i].p, &out[i].n)) < 0)
			break;
	}
	for (; i < ncol; i++) {        /* a malformed tail reads as NULL */
		out[i].cls = XSTORE_C_NULL; out[i].i = 0; out[i].r = 0;
		out[i].p = NULL; out[i].n = 0;
	}
	return ncol;
}

/* Number of payload columns in a record (0 for an empty / NULL one). */
int
xstore_rec_ncol(const uint8_t *rec, int reclen)
{
	return (rec == NULL || reclen < 1) ? 0 : rec[0];
}

/* ---- native autocommit write path (VDBE-free INSERT) ------------- */

int
xstore_table_id(bt_t *bt, const char *name, uint32_t *tableid)
{
	uint32_t id;
	int i, err;
	if (bt == NULL || name == NULL) return 0;
	if (xs_tcat_get(bt, name, &id, NULL)) {      /* this thread's copy */
		if (tableid) *tableid = id;
		return 1;
	}
	/* Cache first: a CREATE on another connection reserves the table-id
	 * in the in-process cache before its catalog row is persisted to the
	 * B-tree (xs_put parks on the WAL ack), so a concurrent xs_cat_lookup
	 * would miss it.  The cache makes the table visible immediately to
	 * every connection on the shared bt. */
	xs_cat_lock();
	for (i = 0; i < g_cat_n; i++)
		if (g_cat[i].bt == bt && strcasecmp(g_cat[i].name, name) == 0) {
			id = g_cat[i].tableid;
			xs_tcat_put_locked(&g_cat[i]);
			xs_cat_unlock();
			if (tableid) *tableid = id;
			return 1;
		}
	xs_cat_unlock();
	id = xs_cat_lookup(bt, name, &err);
	if (err != 0) return err;   /* unread: not known to be absent */
	if (id == 0) return 0;      /* unknown */
	xs_cat_prime(bt, name, id);
	if (tableid) *tableid = id;
	return 1;
}

/* Map a declared type string to a SQLite type-affinity letter, using
 * SQLite's substring rules (INT->i, CHAR/CLOB/TEXT->t, BLOB or empty
 * ->b, REAL/FLOA/DOUB->r, else numeric->r). */
static char
xs_affinity_of(const char *ty)
{
	char up[64]; int i;
	if (ty == NULL || ty[0] == '\0') return 'b';
	for (i = 0; ty[i] && i < (int)sizeof up - 1; i++) {
		char c = ty[i];
		up[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
	}
	up[i] = '\0';
	if (strstr(up, "INT")) return 'i';
	if (strstr(up, "CHAR") || strstr(up, "CLOB") || strstr(up, "TEXT")) return 't';
	if (strstr(up, "BLOB")) return 'b';
	if (strstr(up, "REAL") || strstr(up, "FLOA") || strstr(up, "DOUB")) return 'r';
	return 'r';   /* NUMERIC and friends */
}

/* Read the catalog value (name + optional NUL + coldefs) for the newest
 * live entry whose name matches `name`.  Returns the value length with
 * the bytes copied to `out` (capacity `cap`), 0 if not found, or a
 * read's XTC_E_* code. */
static int
xs_cat_value(bt_t *bt, const char *name, uint8_t *out, int cap)
{
	bt_cursor_t *cur = NULL;
	size_t want = strlen(name);
	int64_t last_rowid = 0; int have_last = 0, got = 0, rc;

	xtc_atomic_fetch_add_u64(&g_cat_scans, 1, xtc_mo_relaxed);
	if ((rc = xs_cat_open(bt, XS_CAT_TABLEID, &cur)) != XTC_OK)
		return rc == XTC_E_NOTFOUND ? 0 : rc;
	for (;;) {
		const void *k = NULL, *vv = NULL; uint16_t klen = 0, vl = 0;
		const uint8_t *kb, *vb; int64_t rid; size_t nlen, j;
		rc = xs_cat_next(cur, &k, &klen, &vv, &vl);
		if (rc != XTC_OK && rc != XTC_E_NOTFOUND) {
			got = rc;
			break;
		}
		if (rc != XTC_OK || klen != XS_VKLEN) break;
		kb = (const uint8_t *)k; vb = (const uint8_t *)vv;
		if (dec_tableid(kb) != XS_CAT_TABLEID) break;
		rid = dec_rowid(kb);
		if (have_last && rid == last_rowid) continue;
		last_rowid = rid; have_last = 1;
		if (vl >= 1 && (vb[0] & XS_F_DELETED)) continue;
		nlen = (size_t)(vl >= 1 ? vl - 1 : 0);
		for (j = 0; j < nlen; j++) if (vb[1 + j] == 0) { nlen = j; break; }
		if (nlen == want && strncasecmp((const char *)vb + 1, name, want) == 0) {
			int n = (int)(vl >= 1 ? vl - 1 : 0);
			if (n > cap) n = cap;
			if (n > 0) memcpy(out, vb + 1, (size_t)n);
			got = n;
			break;
		}
	}
	bt_cursor_close(cur);
	return got;
}

static int xs_table_schema_parse(bt_t *bt, const char *name,
    xstore_col_t *cols, int cap);

/*
 * Parsed-schema cache.  Every write statement and most planner paths ask
 * for the table's columns; parsing the coldefs string each time was ~4%
 * of a TPC-C connection's CPU.  A thread-local copy keyed by (bt, name)
 * stays valid while g_catc_gen is unchanged (bumped by every catalog
 * change, as for t_cat).  Only complete schemas (ncol <= the slot) are
 * cached, and a hit copies at most `cap` columns -- the same truncation
 * the parse applies.
 */
#define XS_TSCH_N    8
#define XS_TSCH_COLS 32
static _Thread_local struct {
	uint64_t     gen;
	bt_t        *bt;
	char         name[64];
	int          ncol;
	xstore_col_t cols[XS_TSCH_COLS];
} t_sch[XS_TSCH_N];
static _Thread_local unsigned t_sch_next;

int
xstore_table_schema(bt_t *bt, const char *name, xstore_col_t *cols, int cap)
{
	uint64_t g;
	int k, n;
	if (bt == NULL || name == NULL || cols == NULL || cap <= 0)
		return -1;
	g = xtc_atomic_load_u64(&g_catc_gen, xtc_mo_acquire);
	for (k = 0; k < XS_TSCH_N; k++)
		if (t_sch[k].gen == g && t_sch[k].bt == bt &&
		    strcasecmp(t_sch[k].name, name) == 0) {
			n = t_sch[k].ncol < cap ? t_sch[k].ncol : cap;
			memcpy(cols, t_sch[k].cols, (size_t)n * sizeof *cols);
			return n;
		}
	n = xs_table_schema_parse(bt, name, cols, cap);
	/* Cache only a complete parse (fewer columns than cap: not cut) at
	 * the generation read BEFORE it, so a concurrent DDL invalidates. */
	if (n > 0 && n < cap && n <= XS_TSCH_COLS && strlen(name) < 64) {
		k = (int)(t_sch_next++ % XS_TSCH_N);
		t_sch[k].gen = g;
		t_sch[k].bt = bt;
		snprintf(t_sch[k].name, sizeof t_sch[k].name, "%s", name);
		t_sch[k].ncol = n;
		memcpy(t_sch[k].cols, cols, (size_t)n * sizeof *cols);
	}
	return n;
}

static int
xs_table_schema_parse(bt_t *bt, const char *name, xstore_col_t *cols, int cap)
{
	uint8_t val[1024];
	char    cached[512];
	int vlen, i, ncol = 0, pass, r;
	const char *cd; int cdlen;

	if (bt == NULL || name == NULL || cols == NULL || cap <= 0) return -1;

	/* Prefer the in-process cache's coldefs: a CREATE on another
	 * connection caches the schema the instant it reserves the table-id,
	 * before the persisted catalog row is necessarily visible here.  A
	 * miss (a store reopened from disk) primes the cache from the
	 * persisted catalog once, so later statements hit.  Fall back to the
	 * persisted value only if the schema does not fit the cache. */
	cached[0] = '\0';
	for (pass = 0; pass < 2 && cached[0] == '\0'; pass++) {
		uint32_t pid;
		int hit = 0;
		if (xs_tcat_get(bt, name, NULL, cached))
			break;
		xs_cat_lock();
		for (i = 0; i < g_cat_n; i++)
			if (g_cat[i].bt == bt && strcasecmp(g_cat[i].name, name) == 0) {
				if (g_cat[i].coldefs[0] != '\0')
					snprintf(cached, sizeof cached, "%s", g_cat[i].coldefs);
				xs_tcat_put_locked(&g_cat[i]);
				hit = 1;
				break;
			}
		xs_cat_unlock();
		if (hit)
			break;                      /* cached (coldefs may not fit) */
		if (pass == 0 && (r = xstore_table_id(bt, name, &pid)) <= 0)
			return r;                   /* no such table, or unread */
	}
	if (cached[0] != '\0') {
		cd = cached;
		cdlen = (int)strlen(cached);
	} else {
		vlen = xs_cat_value(bt, name, val, (int)sizeof val);
		if (vlen <= 0) return vlen;      /* none recorded, or unread */
		/* The coldefs follow the name and a NUL. */
		for (i = 0; i < vlen; i++) if (val[i] == 0) break;
		if (i >= vlen - 1) return 0;          /* no schema recorded */
		cd = (const char *)val + i + 1;
		cdlen = vlen - (i + 1);
	}

	/* Parse comma-separated column defs: "<name>[ <type...>]". */
	{
		int p = 0;
		while (p < cdlen && ncol < cap) {
			int start = p, ce, nn = 0, tn = 0;
			xstore_col_t *c = &cols[ncol];
			while (p < cdlen && cd[p] != ',') p++;   /* one column def */
			ce = p;
			if (p < cdlen) p++;                      /* skip comma */
			/* trim leading space */
			while (start < ce && cd[start] == ' ') start++;
			/* name = up to the first space */
			memset(c, 0, sizeof *c);
			while (start < ce && cd[start] != ' ' && nn < (int)sizeof c->name - 1)
				c->name[nn++] = cd[start++];
			c->name[nn] = '\0';
			while (start < ce && cd[start] == ' ') start++;
			/* the rest is the declared type */
			while (start < ce && tn < (int)sizeof c->decltype - 1)
				c->decltype[tn++] = cd[start++];
			c->decltype[tn] = '\0';
			c->is_pk = (ncol == 0);          /* column 0 is the rowid PK */
			c->hidden = (ncol == 0 &&
			    strcmp(c->name, "__rowid__") == 0); /* implicit rowid */
			c->affinity = c->is_pk ? 'i' : xs_affinity_of(c->decltype);
			ncol++;
		}
	}
	return ncol > 0 ? ncol : 0;
}

int
xstore_max_rowid(bt_t *bt, uint32_t tableid, int64_t *out)
{
	/* The largest rowid in the table sorts last within the table's key
	 * range (keys are (tableid, rowid ^ signbit, ~ts), big-endian, so
	 * rowids sort numerically).  Tombstoned versions count too, so a
	 * reused rowid never collides with a deleted one's slot.  Empty
	 * table (or only negative rowids) -> 0, so the first auto-rowid is 1
	 * -- exactly the max(0, ...) the forward scan computed. */
	/* O(log n): the largest key below [tableid][0xFF x 17] is the
	 * table's last key -- its largest rowid (any version, tombstones
	 * included, as before).  1.56 walked every version of the table with
	 * a forward cursor, so every native INSERT cost O(table): 4 ms on an
	 * empty table, 16.5 ms at 400k rows (review156 P1). */
	uint8_t hi[XS_VKLEN + 1], k[XS_VKLEN + 8];
	uint16_t klen = 0;
	int i, rc;

	*out = 0;
	if (bt == NULL) return XTC_OK;
	for (i = 3; i >= 0; i--) hi[3 - i] = (uint8_t)(tableid >> (8 * i));
	memset(hi + 4, 0xFF, sizeof hi - 4);
	rc = bt_seek_lt(bt, hi, (uint16_t)sizeof hi, k, (uint16_t)sizeof k,
	    &klen, NULL, 0, NULL);
	if (rc == XTC_OK && xs_row_fault())
		rc = XTC_E_IO;
	/* A descent that failed is not an empty table: the caller would
	 * hand out rowid 1 over a committed row. */
	if (rc == XTC_E_NOTFOUND)
		return XTC_OK;               /* nothing below: empty */
	if (rc != XTC_OK && XTC_DST_BUG(XTC_DST_BUG_MAXROWID))
		return XTC_OK;  /* planted 85: a failed descent is an empty table */
	if (rc != XTC_OK)
		return xs_rd_count(rc);
	if (klen != XS_VKLEN || dec_tableid(k) != tableid)
		return XTC_OK;               /* empty table */
	*out = dec_rowid(k) > 0 ? dec_rowid(k) : 0;
	return XTC_OK;
}

int
xstore_rowid_split_points(bt_t *bt, uint32_t tableid, int64_t lo, int64_t hi,
                          int n, int64_t *out)
{
	/* Sample the version-key separators inside the table's [lo, hi)
	 * rowid range (RethinkDB get_distribution), decode each key's rowid
	 * and keep the strictly increasing ones inside (lo, hi).  Versions of
	 * one rowid sort together, so a separator inside a run of versions
	 * decodes to that rowid: a cut at it puts all of the rowid's
	 * versions in the morsel that starts there.  Thinned evenly to n.
	 *
	 * Depth 1 (the root and its children, 1 + root-fanout page reads)
	 * yields the boundaries of the nodes two levels down -- hundreds on
	 * a 4 KiB page, plenty for a few dozen cuts.  Only a table that is a
	 * narrow slice of a shared tree finds fewer; then go one level
	 * deeper.  That costs little: depth d found < n boundaries in range
	 * exactly when the range meets <= n + 1 nodes one level further
	 * down, so the next pass reads at most those plus their ancestors. */
	enum { CAP = 256 };
	uint8_t klo[XS_VKLEN], khi[XS_VKLEN];
	uint8_t *keys;
	uint16_t lens[CAP];
	uint32_t got, i;
	int64_t *tmp;
	int m = 0, k, depth;

	if (bt == NULL || out == NULL || n <= 0 || hi <= lo + 1) return 0;
	enc_vkey(tableid, lo, ~(uint64_t)0, klo);   /* first version of lo */
	enc_vkey(tableid, hi, ~(uint64_t)0, khi);   /* first version of hi */
	keys = (uint8_t *)malloc((size_t)CAP * XS_VKLEN);
	tmp = (int64_t *)malloc(sizeof *tmp * CAP);
	if (keys == NULL || tmp == NULL) { free(keys); free(tmp); return 0; }
	for (depth = 1; depth <= 4 && m < n; depth++) {
		got = 0;
		m = 0;
		/* A hint: with fewer cut points the caller splits uniformly. */
		if (bt_sample_keys(   /* RDERR_OK: a hint */
		    bt, klo, XS_VKLEN, khi, XS_VKLEN, depth, keys,
		    XS_VKLEN, lens, CAP, &got) != XTC_OK)
			break;
		for (i = 0; i < got; i++) {
			uint8_t *kp = keys + (size_t)i * XS_VKLEN;
			int64_t r;
			/* Separators are truncated (the shortest key between two
			 * leaves, 2.0), so most are shorter than a version key.
			 * Zero-padded, a separator still lies between the keys it
			 * separates, so its rowid is a cut point; one too short to
			 * name the table carries none. */
			if (lens[i] < 4 || lens[i] > XS_VKLEN)
				continue;
			memset(kp + lens[i], 0, XS_VKLEN - lens[i]);
			if (dec_tableid(kp) != tableid)
				continue;
			r = dec_rowid(kp);
			if (r <= lo || r >= hi || (m > 0 && r <= tmp[m - 1]))
				continue;
			tmp[m++] = r;
		}
	}
	for (k = 0; k < n && k < m; k++)
		out[k] = tmp[m <= n ? k : (int)(((int64_t)(k + 1) * m) / (n + 1))];
	free(keys);
	free(tmp);
	return k;
}

int
xstore_put_rec(bt_t *bt, uint32_t tableid, int64_t rowid,
               const uint8_t *rec, int reclen)
{
	uint64_t ts = 0;
	int rc;
	if (bt == NULL || rec == NULL || reclen < 1) return -1;
	rc = xs_put_ts(bt, tableid, rowid, rec, reclen, 0, 0, 0, &ts, NULL, 0);
	return rc == SX_OK ? 0 : -1;
}

int
xstore_delete_rec(bt_t *bt, uint32_t tableid, int64_t rowid)
{
	/* A delete is a tombstone version (deleted=1) at a fresh commit
	 * timestamp -- the same thing the vtab xUpdate DELETE path writes. */
	uint64_t ts = 0;
	int rc;
	if (bt == NULL) return -1;
	rc = xs_put_ts(bt, tableid, rowid, NULL, 0, 1, 0, 0, &ts, NULL, 0);
	return rc == SX_OK ? 0 : -1;
}

#if defined(SQLXTC_VTAB)
static int
xs_filter(xsql_vtab_cursor *pc, int idxNum, const char *idxStr,
    int argc, xsql_value **argv)
{
	xstore_cursor_t *c = (xstore_cursor_t *)pc;
	xstore_vtab_t *v = (xstore_vtab_t *)pc->pVtab;
	uint64_t snap;
	(void)idxStr;

	if (c->btc != NULL) {           /* restart: drop any prior scan cursor */
		bt_cursor_close(c->btc);
		c->btc = NULL;
	}
	xs_enter(v);
	snap = xtc_atomic_load_u64(&v->ctx->read_snap, xtc_mo_relaxed);
	if (snap == 0) {
		if (v->ctx->in_txn &&
		    v->ctx->isolation >= XS_ISO_REPEATABLE_READ)
			snap = v->ctx->txn_snap;   /* one snapshot for the txn */
		else
			/* READ COMMITTED / READ UNCOMMITTED, or autocommit:
			 * a fresh latest-committed snapshot for this statement. */
			snap = xtc_atomic_load_u64(&g_xclock, xtc_mo_relaxed);
	}
	c->snap = snap;
	c->have_last = 0;
	c->eof = 1;

	if (idxNum == 1 && argc >= 1) {
		/* Point lookup.  Read-your-writes first: a write buffered in
		 * this open transaction supersedes the committed version. */
		bt_cursor_t *cur = NULL;
		uint8_t startk[XS_VKLEN];
		const void *k = NULL, *vv = NULL;
		uint16_t klen = 0, vl = 0;
		int64_t want = xsql_value_int64(argv[0]);
		c->point = 1;
		if (v->ctx->in_txn && v->ctx->serializable) {
			rset_add(v->ctx, c->tableid, want);   /* read set for serializable validation */
			ssi_record_read(v->ctx->ssi, c->tableid, want);
		}
		if (v->ctx->in_txn) {
			const xs_wrec_t *w = wbuf_find(v->ctx, c->tableid, want);
			if (w != NULL) {
				if (!w->deleted) {
					(void)xs_wrec_ensure(v->ctx, (xs_wrec_t *)w);  /* un-spill if needed */
					c->rowid = want;
					c->vlen = w->len;
					if (w->len && w->data) memcpy(c->val, w->data, w->len);
					c->eof = 0;
				}
				return SX_OK;       /* buffered write decides it */
			}
		}
		/* Else the newest committed version at-or-before the snapshot. */
		enc_vkey(c->tableid, want, snap, startk);
		if (bt_cursor_open(c->bt, startk, XS_VKLEN, &cur) != XTC_OK)
			return SX_OK;
		if (bt_cursor_next(cur, &k, &klen, &vv, &vl) == XTC_OK &&
		    klen == XS_VKLEN &&
		    dec_tableid((const uint8_t *)k) == c->tableid &&
		    dec_rowid((const uint8_t *)k) == want &&
		    !(vl >= 1 && (((const uint8_t *)vv)[0] & XS_F_DELETED))) {
			xs_stash(c, want, (const uint8_t *)vv, vl);
			c->eof = 0;
		}
		bt_cursor_close(cur);
		return SX_OK;
	}
	c->point = 0;
	/* Range bounds from best_index (idxNum bit 2 = lo, bit 4 = hi). */
	c->has_lo = c->has_hi = 0;
	{
		int ai = 0;
		if ((idxNum & 2) && argc > ai) { c->lo = xsql_value_int64(argv[ai++]); c->has_lo = 1; }
		if ((idxNum & 4) && argc > ai) { c->hi = xsql_value_int64(argv[ai++]); c->has_hi = 1; }
	}
	if (v->ctx->in_txn && v->ctx->serializable) {
		int64_t rlo = c->has_lo ? c->lo : INT64_MIN;
		int64_t rhi = c->has_hi ? c->hi : INT64_MAX;
		v->ctx->did_scan = 1;     /* conservative outgoing edge */
		ssi_record_range(v->ctx->ssi, c->tableid, rlo, rhi);  /* precise incoming predicate */
	}
	xs_advance(c);
	return SX_OK;
}
#endif /* SQLXTC_VTAB */

#if defined(SQLXTC_VTAB)
static int
xs_next(xsql_vtab_cursor *pc)
{
	xstore_cursor_t *c = (xstore_cursor_t *)pc;
	if (c->point) { c->eof = 1; return SX_OK; }
	xs_advance(c);
	return SX_OK;
}
#endif /* SQLXTC_VTAB */

#if defined(SQLXTC_VTAB)
static int
xs_eof(xsql_vtab_cursor *pc)
{
	return ((xstore_cursor_t *)pc)->eof;
}
#endif /* SQLXTC_VTAB */

#if defined(SQLXTC_VTAB)
static int
xs_column(xsql_vtab_cursor *pc, xsql_context *ctx, int i)
{
	xstore_cursor_t *c = (xstore_cursor_t *)pc;
	if (i == 0)
		xsql_result_int64(ctx, c->rowid);
	else
		xs_rec_result(ctx, c->val, c->vlen, i - 1);
	return SX_OK;
}
#endif /* SQLXTC_VTAB */

#if defined(SQLXTC_VTAB)
static int
xs_rowid(xsql_vtab_cursor *pc, xsql_int64 *pRowid)
{
	*pRowid = ((xstore_cursor_t *)pc)->rowid;
	return SX_OK;
}
#endif /* SQLXTC_VTAB */

/* Write one version of `rowid`: a tombstone if deleted, else a copy of
 * `blob`, stamped with a fresh commit timestamp (autocommit). */
/* Dispatch one WAL record to the log durably: group commit on a loop
 * (parks the fiber on the writer's ack), synchronous append off a loop.
 * No-op when no WAL is attached. */
static int
xs_wal_emit(const uint8_t *rec, size_t sz, uint64_t *lsn)
{
	int rc;
	*lsn = 0;
	if (g_xwal == NULL)
		return XTC_OK;
	/* Masked: wal_commit delivers a latched kill at its end, which would
	 * leave g_wal_busy raised and the checkpoint gate waiting forever. */
	(void)xtc_mask_enter();
	xtc_atomic_fetch_add_i32(&g_wal_busy, 1, xtc_mo_acq_rel);
	if (xtc_pid_is_none(xtc_self()))
		rc = wal_commit_sync(g_xwal, rec, (uint32_t)sz, lsn);
	else
		rc = wal_commit(g_xwal, rec, (uint32_t)sz, lsn);
	xtc_atomic_fetch_sub_i32(&g_wal_busy, 1, xtc_mo_acq_rel);
	(void)xtc_mask_leave();
	return rc;
}

/* Sleep briefly: on a proc park the fiber, off a loop block the thread. */
static void
xs_nap(void)
{
	if (xtc_pid_is_none(xtc_self()))
		(void)xtc_thrd_sleep_ns(200LL * 1000);
	else
		(void)xtc_proc_sleep(200LL * 1000);
}

/* Caller holds the commit lock: wait (dropping and re-taking it) while a
 * checkpoint holds the gate. */
static void
xs_gate_wait_locked(void)
{
	while (g_ckpt_gate) {
		xs_commit_unlock();
		xs_nap();
		xs_commit_lock();
	}
}

/*
 * Quiesce writers for a checkpoint: close the gate (new commits and
 * spills wait), then wait until no commit is in flight and no log write
 * is outstanding.  On return the tree and the log are stable until
 * xstore_quiesce_end; readers keep running.  Call from a proc (or off a
 * loop); never while holding an engine lock.
 */
static int g_ock_cancel; /* xstore_checkpoint_cancel */

/* Take the checkpoint gate and drain the commits in flight.  With
 * `cancellable` XS_QB_CKPT, give up (returning 0, gate not taken) once
 * xstore_checkpoint_cancel is set; with XS_QB_STOP, once
 * xstore_quiesce_final has run.  Either way the engine is stopping and
 * its final quiesce holds the gate for good.  Returns 1 when quiesced. */
#define XS_QB_CKPT 1
#define XS_QB_STOP 2
static int
xs_quiesce_begin(int cancellable)
{
	int idle = 0;
	xs_commit_lock();
	while (g_ckpt_gate) {          /* one checkpoint at a time */
		xs_commit_unlock();
		if (((cancellable & XS_QB_CKPT) &&
		    xtc_atomic_load_i32(&g_ock_cancel, xtc_mo_acquire)) ||
		    ((cancellable & XS_QB_STOP) &&
		    xtc_atomic_load_i32(&g_xs_stopped, xtc_mo_acquire)))
			return 0;
		xs_nap();
		xs_commit_lock();
	}
	g_ckpt_gate = 1;
	xs_commit_unlock();
	while (!idle) {
		xs_commit_lock();
		idle = g_inflight == NULL &&
		    xtc_atomic_load_i32(&g_wal_busy, xtc_mo_acquire) == 0;
		xs_commit_unlock();
		if (!idle)
			xs_nap();
	}
	return 1;
}

void
xstore_quiesce_begin(void)
{
	(void)xs_quiesce_begin(0);
}

/* The engine's last quiesce: mark the store stopped, so a DDL statement
 * that meets it fails instead of waiting for a gate that never opens,
 * then quiesce as xstore_quiesce_begin does. */
void
xstore_quiesce_final(void)
{
	xtc_atomic_store_i32(&g_xs_stopped, 1, xtc_mo_release);
	(void)xs_quiesce_begin(0);
}

/* How long one wait for the DDL lock lasts before the stopped flag is
 * checked again. */
#define XS_DDL_WAIT_NS (10LL * 1000 * 1000)

/* One DDL statement at a time (see xstore.h).  Ranks before every
 * other engine lock: a DDL statement takes it first and holds it across
 * the cat and commit locks its steps take, and the parks of an index
 * build.  The wait is cut into XS_DDL_WAIT_NS slices so a stopped store
 * is noticed: its holder may be parked at the closed gate for good. */
int
xstore_ddl_lock(void)
{
	xtc_amutex_t *m = xs_lk_get(XS_LK_DDL);
	(void)xtc_mask_enter();
	for (;;) {
		if (xtc_atomic_load_i32(&g_xs_stopped, xtc_mo_acquire))
			break;
		if (xtc_amutex_lock(m, XS_DDL_WAIT_NS) != XTC_OK)
			continue;
		/* Stopped while this waited: the statement could not commit. */
		if (!xtc_atomic_load_i32(&g_xs_stopped, xtc_mo_acquire))
			return XTC_OK;
		(void)xtc_amutex_unlock(m);
		break;
	}
	(void)xtc_mask_leave();
	return XTC_E_STATE;
}

void
xstore_ddl_unlock(void)
{
	xs_unlk(XS_LK_DDL);
}

void
xstore_quiesce_end(void)
{
	xs_commit_lock();
	g_ckpt_gate = 0;
	xs_commit_unlock();
}

/* 1 once the attached log has fail-stopped (a write or fsync failed):
 * the engine is read-only until restart.  See wal_commit. */
int
xstore_wal_failed(void)
{
	return g_xwal != NULL && wal_failed(g_xwal);
}

/* Log a single committed version (the autocommit single-statement
 * path); same record layout as the multi-version xs_wal_log.  Returns
 * XTC_OK with the assigned LSN in *lsn, or an error (nothing durable). */
static int
xs_wal_put(uint64_t ts, uint32_t tableid, int64_t rowid, const void *val,
    int vlen, int deleted, uint64_t *lsn)
{
	uint8_t rec[64 + XS_VMAX];        /* one XL_UPDATE + a trailing XL_COMMIT */
	uint8_t *p = rec;
	xl_hdr_t h;
	xl_body_t b;
	int n;
	uint16_t vl = (deleted || vlen < 0) ? 0
	    : (uint16_t)(vlen > XS_VMAX ? XS_VMAX : vlen);

	h.type = XL_UPDATE; h.txn_id = ts; h.prev_lsn = 0;
	memset(&b, 0, sizeof b);
	b.tableid = tableid; b.rowid = rowid; b.commit_ts = ts;
	b.flags = deleted ? XS_F_DELETED : 0;
	b.redo = vl ? val : NULL; b.redo_len = vl;
	if ((n = xl_enc_update(p, sizeof rec, &h, &b)) < 0)
		return XTC_E_INVAL;
	p += n;
	h.type = XL_COMMIT;               /* this single statement is its own txn */
	if ((n = xl_enc_simple(p, sizeof rec - (size_t)(p - rec), &h)) < 0)
		return XTC_E_INVAL;
	p += n;
	return xs_wal_emit(rec, (size_t)(p - rec), lsn);
}

/* STEAL: log one spilled payload as an XL_UPDATE belonging to a steal
 * transaction that never commits (no XL_COMMIT for steal_txn).  Recovery
 * therefore redoes it into the staging area and then undoes it with a
 * CLR -- removing the uncommitted spill -- whether the owning SQL
 * transaction committed (its real versions are logged separately under
 * the commit timestamp) or not. */
static void
xs_wal_emit_stage(uint64_t steal_txn, int64_t stage_id, const void *val, uint16_t vlen)
{
	uint8_t rec[64 + XS_VMAX];
	xl_hdr_t h;
	xl_body_t b;
	int n;
	uint64_t lsn;

	h.type = XL_UPDATE; h.txn_id = steal_txn; h.prev_lsn = 0;
	memset(&b, 0, sizeof b);
	b.tableid = XS_STAGE_TID; b.rowid = stage_id; b.commit_ts = 0;
	b.redo = vlen ? val : NULL; b.redo_len = vlen;
	/* Best-effort: a failed log write latches the log failed, so the
	 * owning transaction's commit record then fails too. */
	if ((n = xl_enc_update(rec, sizeof rec, &h, &b)) > 0)
		(void)xs_wal_emit(rec, (size_t)n, &lsn);
}

/*
 * SMO physiological logging (ARIES nested top action).  A B-tree split
 * or root growth calls these hooks (registered via xstore_register_smo)
 * for each page it modifies; we log a full-page after-image (XL_PAGE,
 * redo-only, page-LSN at the front) per page, then a dummy compensation
 * record closing the nested top action.  On a crash mid-SMO, recovery
 * replays the XL_PAGE images (page-LSN gated, idempotent) to repair the
 * torn structure in place; the dummy CLR records that a completed SMO is
 * redone to completion and never half-undone (Stasis NTA model,
 * the design notes sec 2.7).  No-op without a log.
 */
static uint64_t
xs_smo_begin(void *user)
{
	(void)user;
	if (g_xwal == NULL)
		return 0;
	/* The NTA token is the log's current durable high-water LSN: the
	 * SMO's records land after it, so a dummy CLR's undo_next_lsn can
	 * point here to skip the interior physical records on undo. */
	return wal_durable_lsn(g_xwal);
}

static uint64_t
xs_smo_page(void *user, bm_pid_t pid, const void *image,
    uint32_t page_size, uint64_t lsn)
{
	uint8_t *rec;
	xl_hdr_t h;
	int n;
	size_t cap;
	uint64_t rlsn = 0;

	(void)user;
	if (g_xwal == NULL || image == NULL || page_size == 0 ||
	    page_size > 0xFFFFu)
		return 0;
	cap = (size_t)XL_HDR_LEN + xl_page_size((uint16_t)page_size);
	if ((rec = malloc(cap)) == NULL)
		return 0;               /* best-effort: a missed image means the
		                         * torn page is repaired by rebuild instead */
	h.type = XL_PAGE; h.txn_id = 0; h.prev_lsn = lsn;
	if ((n = xl_enc_page(rec, cap, &h, (uint32_t)pid, image,
	    (uint16_t)page_size)) > 0)
		(void)xs_wal_emit(rec, (size_t)n, &rlsn);   /* 0 on failure */
	free(rec);
	return rlsn;
}

/*
 * Physiological after-image of a PLAIN (non-split) leaf.  Identical
 * wire form to xs_smo_page (an XL_PAGE, gated on apply by the image
 * record's own WAL LSN), but logged for an in-leaf insert that did NOT
 * split -- so in-place recovery can repair a torn NON-split leaf from
 * its image instead of a logical redo mis-descending the torn page and
 * losing its whole key range (the the design notes S3 trap).  No
 * nested-top-action bracket: a single-leaf change is atomic under the
 * double-write buffer.  txn_id 0 (the image belongs to no one txn).
 * Returns the image record's WAL LSN so btree.c can stamp the live page.
 */
static uint64_t
xs_leaf_page(void *user, bm_pid_t pid, const void *image,
    uint32_t page_size, uint64_t lsn)
{
	return xs_smo_page(user, pid, image, page_size, lsn);
}

static void
xs_smo_end(void *user, uint64_t token)
{
	uint8_t clr[64];
	xl_hdr_t h;
	xl_body_t b;
	int n;
	uint64_t lsn;

	(void)user;
	if (g_xwal == NULL)
		return;
	/* Dummy CLR closing the nested top action: redo-only, undo_next_lsn
	 * = the LSN before the SMO so undo of an enclosing loser skips the
	 * interior physical (XL_PAGE) records.  txn_id 0 (the SMO belongs to
	 * no single transaction). */
	h.type = XL_CLR; h.txn_id = 0; h.prev_lsn = 0;
	memset(&b, 0, sizeof b);
	b.undo_next_lsn = token;
	if ((n = xl_enc_clr(clr, sizeof clr, &h, &b)) > 0)
		(void)xs_wal_emit(clr, (size_t)n, &lsn);
}

/* Install the SMO physiological-logging hook on the B-tree layer.  The
 * engine calls this once after attaching the WAL; the hook is global
 * (the store is process-global), matching bt_set_close_hook.  Passing 0
 * removes it (e.g. for a tree that should rebuild logically). */
void
xstore_register_smo(int enable)
{
	if (enable) {
		bt_smo_hook_t h;
		h.user = NULL;
		h.begin = xs_smo_begin;
		h.page = xs_smo_page;
		h.end = xs_smo_end;
		h.leaf = xs_leaf_page;   /* physiological non-split-leaf images */
		bt_set_smo_hook(&h);
	} else {
		bt_set_smo_hook(NULL);
	}
}

/* STEAL: when buffered payloads exceed the high-water mark, write the
 * oldest ones to the on-disk staging area (and the log) and free them
 * from memory, bounding a large transaction's resident footprint during
 * execution.  Keys stay in wbuf, so read-your-writes and SSI validation
 * are unaffected; a spilled payload is brought back by xs_wrec_ensure.
 * No-op without a log (nothing to recover from). */
static void
xs_spill_payloads(xstore_ctx_t *cx)
{
	int i, gated;

	if (g_xwal == NULL)
		return;
	for (i = 0; i < cx->wn && cx->wbuf_bytes > XS_SPILL_LO; i++) {
		xs_wrec_t *w = &cx->wbuf[i];
		uint8_t key[XS_VKLEN];
		int64_t sid;

		if (w->spilled || w->data == NULL || w->len == 0)
			continue;
		/* A checkpoint is dumping the tree: keep payloads in memory. */
		xs_commit_lock();
		gated = g_ckpt_gate;
		if (!gated) {
			(void)xtc_mask_enter();   /* until g_wal_busy drops */
			xtc_atomic_fetch_add_i32(&g_wal_busy, 1,
			    xtc_mo_acq_rel);
		}
		xs_commit_unlock();
		if (gated)
			break;
		sid = (int64_t)xtc_atomic_fetch_add_u64(&g_stage_seq, 1,
		    xtc_mo_relaxed);
		enc_vkey(XS_STAGE_TID, sid, 0, key);
		if (bt_insert(cx->bt, key, XS_VKLEN, w->data, w->len) != XTC_OK) {
			xtc_atomic_fetch_sub_i32(&g_wal_busy, 1,
			    xtc_mo_acq_rel);
			(void)xtc_mask_leave();
			continue;                  /* keep it in memory on failure */
		}
		if (cx->steal_txn == 0)
			cx->steal_txn = xtc_atomic_fetch_add_u64(&g_steal_txn,
			    1, xtc_mo_relaxed);
		xs_wal_emit_stage(cx->steal_txn, sid, w->data, w->len);
		xtc_atomic_fetch_sub_i32(&g_wal_busy, 1, xtc_mo_acq_rel);
		free(w->data); w->data = NULL; w->spilled = 1; w->stage_id = sid;
		cx->wbuf_bytes -= w->len;
		(void)xtc_mask_leave();
	}
}

static int xs_newer_than(bt_t *bt, uint32_t tableid, int64_t rowid,
    uint64_t base);

/*
 * In-flight commit registry.  A commit takes its timestamp and registers
 * its write set here under the commit lock, then logs (parking on the
 * group-commit fsync) and applies its versions WITHOUT the lock, and
 * unregisters when the versions are in the tree (or the commit failed).
 * The write-write check runs under the same lock, so a key written by a
 * commit that has a timestamp but whose versions are not in the tree yet
 * is still seen as a conflict.  Entries live on the committing fiber's
 * stack for the duration of its commit.
 */
struct xs_inflight {
	uint64_t            ts;
	const xs_wrec_t    *ws;     /* txn write set (wn entries), or NULL */
	int                 wn;
	xstore_ctx_t       *cx;     /* the committing txn's ctx (ws != NULL) */
	uint32_t            tid;    /* single-key autocommit write (ws == NULL) */
	int64_t             rid;
	const uint8_t      *rec;    /* its record (NULL for a delete) */
	int                 reclen;
	int done; /* applied or failed; unlink pending */
	struct xs_inflight *next;
};
static int
xs_inflight_has(const struct xs_inflight *e, uint32_t tid, int64_t rid)
{
	int i;
	if (e->ws == NULL)
		return e->tid == tid && e->rid == rid;
	for (i = 0; i < e->wn; i++)
		if (e->ws[i].tableid == tid && e->ws[i].rowid == rid)
			return 1;
	return 0;
}

/* Caller holds the commit lock.  Mint the timestamp and link `e`; stamp
 * the tables it writes (the commit-time UNIQUE re-check skip). */
static void
xs_inflight_add_locked(struct xs_inflight *e)
{
	int i;
	e->ts = hlc_tick();
	e->next = g_inflight;
	g_inflight = e;
	if (e->ws == NULL) {
		g_tab_cts[e->tid % XS_TAB_TS_N] = e->ts;
		g_row_cts[xs_row_slot(e->tid, e->rid)] = e->ts;
	} else {
		for (i = 0; i < e->wn; i++) {
			g_tab_cts[e->ws[i].tableid % XS_TAB_TS_N] = e->ts;
			g_row_cts[xs_row_slot(e->ws[i].tableid, e->ws[i].rowid)] = e->ts;
		}
	}
}

/* Caller holds the commit lock: is a commit writing (tid, rid) in flight? */
static int
xs_inflight_writes(uint32_t tid, int64_t rid)
{
	const struct xs_inflight *e;
	for (e = g_inflight; e != NULL; e = e->next)
		if (xs_inflight_has(e, tid, rid))
			return 1;
	return 0;
}

/* Caller holds the commit lock.  Advance the read horizon to just below
 * the oldest commit still in flight (or to the clock when none is): every
 * timestamp at or below it is durable and applied.  Monotonic. */
static void
xs_visible_catchup_locked(void)
{
	const struct xs_inflight *e;
	uint64_t v = xtc_atomic_load_u64(&g_xclock, xtc_mo_relaxed);
	for (e = g_inflight; e != NULL; e = e->next)
		if (e->ts - 1 < v)
			v = e->ts - 1;
	if (v > xtc_atomic_load_u64(&g_xvisible, xtc_mo_relaxed)) {
		xtc_atomic_store_u64(&g_xvisible, v, xtc_mo_seq_cst);
		g_vis_adv = 1;           /* xs_commit_unlock wakes the waiters */
	}
}

/* Commit lock held: unlink every entry marked done and advance the
 * horizon.  Returns 1 if it advanced.  g_ndone is only a hint (a pruner
 * can unlink an entry before its remover counts it, so the count can
 * read 0 for a moment while one is pending -- that remover's own try
 * then prunes); `force` walks the list regardless. */
static int
xs_inflight_prune_locked(int force)
{
	struct xs_inflight **pp, *e;
	int n = 0;
	if (!force && xtc_atomic_load_i32(&g_ndone, xtc_mo_seq_cst) <= 0)
		return 0;
	for (pp = &g_inflight; (e = *pp) != NULL; ) {
		if (xtc_atomic_load_i32(&e->done, xtc_mo_acquire)) {
			*pp = e->next;
			/* Last touch: once it reads 2 its owner may return. */
			xtc_atomic_store_i32(&e->done, 2, xtc_mo_release);
			n++;
		} else {
			pp = &e->next;
		}
	}
	if (n == 0)
		return 0;
	xtc_atomic_fetch_sub_i32(&g_ndone, n, xtc_mo_seq_cst);
	xs_visible_catchup_locked();
	return 1;
}

/*
 * The commit (applied or failed) leaves the in-flight set.  It no longer
 * waits for the commit lock: at 64 clients the oldest commit's removal
 * queued behind dozens of registrations, and every younger commit's
 * visibility waited on it.  The entry is marked done, and whoever holds
 * the lock unlinks it and advances the horizon (at its acquire and its
 * outermost release, xs_commit_unlock).  No lost prune: we raise g_ndone
 * and then try the lock; a holder releases and then re-reads g_ndone,
 * and both go through the amutex's own mutex, so one of us sees the
 * other.  The entry lives on the caller's stack: the caller must not
 * return before it is unlinked, i.e. before xs_visible() >= e->ts (the
 * horizon never passes a linked entry) -- xs_wait_visible.
 */
static void
xs_inflight_remove(struct xs_inflight *e)
{
	xtc_amutex_t *m = xs_lk_get(XS_LK_COMMIT);
	/* done before the count: a pruner that sees done may drive the count
	 * briefly negative, never spuriously positive. */
	xtc_atomic_store_i32(&e->done, 1, xtc_mo_release);
	xtc_atomic_fetch_add_i32(&g_ndone, 1, xtc_mo_seq_cst);
	if (xtc_amutex_try_lock(m) == XTC_OK) {
		(void)xtc_mask_enter();
		g_commit_depth++;
		xs_commit_unlock();       /* prunes, wakes the covered waiters */
	}
}

/* Return once `e` (removed) is unlinked, for a caller that will not wait
 * for its visibility: an acquire of the commit lock prunes it. */
static void
xs_inflight_unlinked(struct xs_inflight *e)
{
	if (xtc_atomic_load_i32(&e->done, xtc_mo_acquire) == 2)
		return;
	xs_commit_lock();
	if (xtc_atomic_load_i32(&e->done, xtc_mo_acquire) != 2)
		(void)xs_inflight_prune_locked(1);
	xs_commit_unlock();
}

static void xs_vis_forget(void);

static uint64_t
xs_visible(void)
{
	/* seq_cst: snap_pin_visible's re-read is ordered against a GC
	 * scanner's load of the same variable (see the snapshot registry). */
	return xtc_atomic_load_u64(&g_xvisible, xtc_mo_seq_cst);
}

/* Forget every in-flight commit and reopen the checkpoint gate.  Only for
 * points where no commit can be running (log attach/detach, recovery):
 * a run that was abandoned with a committer parked mid-commit (a
 * simulated crash, a test dropping its loop) leaves that committer's
 * stack-resident registry entry linked, which would otherwise dangle
 * and pin the read horizon below it forever. */
static void
xs_commit_state_reset(void)
{
	xs_ilk_reset();
	xs_commit_lock();
	g_inflight = NULL;
	xtc_atomic_store_i32(&g_ndone, 0, xtc_mo_seq_cst);
	g_ckpt_gate = 0;
	xtc_atomic_store_i32(&g_xs_stopped, 0, xtc_mo_release);
	xtc_atomic_store_i32(&g_wal_busy, 0, xtc_mo_relaxed);
	xtc_atomic_store_u64(&g_xvisible,
	    xtc_atomic_load_u64(&g_xclock, xtc_mo_relaxed), xtc_mo_seq_cst);
	xs_commit_unlock();
	xs_vis_forget();
}

/*
 * Visibility waiters.  A commit returns only once its timestamp is
 * visible (xs_wait_visible), which waits for every OLDER commit still in
 * flight.  This was a 20 us sleep poll: at 64 clients it was 18% of the
 * client threads' stacks and a syscall + timer per poll.  Now a waiter
 * queues (sorted by ts) on an xtc_notify, and whoever advances
 * g_xvisible wakes exactly the waiters it covers (xs_vis_wake).
 *
 * No lost wake: a waiter raises g_vw_n and then re-reads g_xvisible; an
 * advancer stores g_xvisible and then reads g_vw_n (all seq_cst), so one
 * of them sees the other; both re-check under g_vw_mu.  g_vw_mu is a
 * leaf lock (never held across a park), held as a proc critical section
 * so involuntary preemption never parks a fiber holding it.  Waiter
 * objects are recycled on g_vw_free and only ever returned by their own
 * waiter, after its wake.
 */
struct xs_vwait {
	uint64_t         ts;
	int              queued;       /* linked on g_vw (under g_vw_mu) */
	xtc_notify_t    *n;
	struct xs_vwait *next;
};
static pthread_mutex_t  g_vw_mu = PTHREAD_MUTEX_INITIALIZER;
static struct xs_vwait *g_vw;          /* queued, ascending ts */
static struct xs_vwait *g_vw_free;
static int g_vw_n; /* waiters inside xs_wait_visible */

static void
xs_vw_lock(void)
{
	xtc_proc_critical_enter();
	(void)pthread_mutex_lock(&g_vw_mu);   /* leaf lock */
}

static void
xs_vw_unlock(void)
{
	(void)pthread_mutex_unlock(&g_vw_mu);
	xtc_proc_critical_leave();
}

/* g_xvisible advanced: wake every queued waiter it covers.  Call with no
 * engine lock held (the wakes are futex syscalls off a loop). */
static void
xs_vis_wake(void)
{
	struct xs_vwait *chain = NULL, *q;
	uint64_t v;
	if (xtc_atomic_load_i32(&g_vw_n, xtc_mo_seq_cst) == 0)
		return;
	xs_vw_lock();
	v = xs_visible();
	while ((q = g_vw) != NULL && q->ts <= v) {
		g_vw = q->next;
		q->queued = 0;
		q->next = chain;
		chain = q;
	}
	xs_vw_unlock();
	/* Unlinked waiters stay parked until signalled, so they cannot
	 * recycle q before the signal below. */
	while ((q = chain) != NULL) {
		chain = q->next;
		(void)xtc_notify_signal(q->n);
	}
}

/* Engine reset (no commit can be running): drop waiters an abandoned run
 * left queued -- their notify queues point into dead fiber stacks, so they
 * must never be signalled.  Leaks those few objects. */
static void
xs_vis_forget(void)
{
	xs_vw_lock();
	g_vw = NULL;
	xtc_atomic_store_i32(&g_vw_n, 0, xtc_mo_seq_cst);
	xs_vw_unlock();
}

/* Return only once commit `ts` is visible to new snapshots, so a session
 * always reads its own commit.  The horizon waits only on OLDER commits
 * still in flight (logging or applying), which never wait on younger
 * ones, so this terminates.  Must be called holding no engine lock.
 * Masked: a waiter killed while queued would leave its object linked. */
static void
xs_wait_visible(uint64_t ts)
{
	struct xs_vwait *q = NULL, **pp;
	if (xs_visible() >= ts)
		return;
	(void)xtc_mask_enter();
	xs_vw_lock();
	xtc_atomic_fetch_add_i32(&g_vw_n, 1, xtc_mo_seq_cst);
	while (xs_visible() < ts) {
		if (q == NULL && (q = g_vw_free) != NULL) {
			g_vw_free = q->next;
		} else if (q == NULL) {
			/* xtc_calloc: the preemption-safe allocator */
			q = xtc_calloc(1, sizeof *q);
			if (q == NULL || xtc_notify_create(&q->n) != XTC_OK) {
				xtc_free(q);         /* no waiter object: poll */
				q = NULL;
				xs_vw_unlock();
				if (xtc_pid_is_none(xtc_self()))
					(void)xtc_thrd_sleep_ns(20LL * 1000);
				else
					(void)xtc_proc_sleep(20LL * 1000);
				xs_vw_lock();
				continue;
			}
		}
		if (!q->queued) {
			q->ts = ts;
			for (pp = &g_vw; *pp != NULL && (*pp)->ts <= ts;
			    pp = &(*pp)->next)
				;
			q->next = *pp;
			*pp = q;
			q->queued = 1;
		}
		xs_vw_unlock();
		(void)xtc_notify_wait(q->n, -1);
		xs_vw_lock();
		/* The waker unlinked q before signalling it; a stray stored
		 * signal finds it still queued and it simply waits again. */
	}
	xtc_atomic_fetch_sub_i32(&g_vw_n, 1, xtc_mo_seq_cst);
	if (q != NULL) {
		q->next = g_vw_free;
		g_vw_free = q;
	}
	xs_vw_unlock();
	(void)xtc_mask_leave();
}

/*
 * First-committer-wins.  Caller holds the commit lock.  A buffered write
 * conflicts if the row has a committed version newer than the snapshot
 * the write was computed against (w->base), or if a commit still in
 * flight writes the same row (it has a timestamp after every snapshot
 * that cannot see it).  Returns 1 on a conflict, or the XTC_E_* code of
 * a read that failed (*bad set to the row's table).
 */
static int
xs_ww_conflict_locked(bt_t *bt, const xs_wrec_t *ws, int wn)
{
	const struct xs_inflight *e;
	int i, r;
	for (i = 0; i < wn; i++) {
		if ((r = xs_newer_than(bt, ws[i].tableid, ws[i].rowid,
		    ws[i].base)) != 0)
			return r;
		for (e = g_inflight; e != NULL; e = e->next)
			if (xs_inflight_has(e, ws[i].tableid, ws[i].rowid))
				return 1;
	}
	return 0;
}

/* The early half of the write-write check, WITHOUT the commit lock: is
 * any row of the write set committed past its base?  Returns 1 on a
 * conflict (certain: committed versions never go away), or a read's
 * XTC_E_* code with *bad set to its table.  Read the horizon *v0 before
 * calling; xs_ww_recheck_locked then finishes. */
static int
xs_ww_probe(bt_t *bt, const xs_wrec_t *ws, int wn, uint32_t *bad)
{
	int i, r;
	for (i = 0; i < wn; i++)
		if ((r = xs_newer_than(bt, ws[i].tableid, ws[i].rowid,
		    ws[i].base)) != 0) {
			*bad = ws[i].tableid;
			return r;
		}
	return 0;
}

/* Commit lock held, after xs_ww_probe passed at horizon v0: the full
 * check only for rows registered (stamped) after v0 -- see g_row_cts.
 * Returns as xs_ww_probe. */
static int
xs_ww_recheck_locked(bt_t *bt, const xs_wrec_t *ws, int wn, uint64_t v0,
    uint32_t *bad)
{
	int i, r;
	for (i = 0; i < wn; i++)
		if (g_row_cts[xs_row_slot(ws[i].tableid, ws[i].rowid)] > v0 &&
		    (r = xs_ww_conflict_locked(bt, &ws[i], 1)) != 0) {
			*bad = ws[i].tableid;
			return r;
		}
	return 0;
}

static int xs_row_live(bt_t *bt, uint32_t tableid, int64_t rowid);

static int
xs_put(bt_t *bt, uint32_t tableid, int64_t rowid, const void *blob, int n, int deleted)
{
	uint64_t ts;
	return xs_put_ts(bt, tableid, rowid, blob, n, deleted, 0, 0, &ts, NULL, 0);
}

/* An INSERT reads "the key is absent".  Caller holds the commit lock.
 * Wait (dropping the lock) until no in-flight commit writes the key --
 * its outcome decides -- then report whether a live version exists
 * (xs_row_live: 1, 0, or a read's XTC_E_* code).
 * In-flight commits never wait on an inserter, so this terminates. */
static int
xs_key_live_locked(bt_t *bt, uint32_t tableid, int64_t rowid)
{
	const struct xs_inflight *e;
	for (;;) {
		for (e = g_inflight; e != NULL; e = e->next)
			if (xs_inflight_has(e, tableid, rowid))
				break;
		if (e == NULL)
			return xs_row_live(bt, tableid, rowid);
		xs_commit_unlock();
		xs_nap();
		xs_commit_lock();
		xs_gate_wait_locked();
	}
}

/* Autocommit write of one version; *out_ts gets its commit timestamp.
 * base != 0: the value was computed from a read at snapshot `base`; if
 * the row has a newer committed version (or a commit writing it is in
 * flight) return SX_BUSY and write nothing (first-committer-wins).
 * insert: the key must not exist; if it does (once any in-flight writer
 * of it has finished) return SX_CONSTRAINT and write nothing.  The
 * check and the registration are one step under the commit lock.
 * vis: return only once the write is visible (the caller holds no
 * engine lock); else only once its in-flight entry is unlinked.
 * A UNIQUE conflict returns SX_CONSTRAINT and a UNIQUE probe that could
 * not run its negative XTC_E_* code, both with *ubad set to the index;
 * a primary-key or write-write check that could not read the row
 * returns its negative XTC_E_* code with *ubad left 0. */
static int
xs_put_ts(bt_t *bt, uint32_t tableid, int64_t rowid, const void *blob, int n,
    int deleted, uint64_t base, int insert, uint64_t *out_ts, uint32_t *ubad,
    int vis)
{
	uint8_t key[XS_VKLEN];
	uint8_t buf[1 + XS_VMAX];
	struct xs_inflight fl;
	struct xs_idx_rt uq[16];
	xs_wrec_t probe;
	uint64_t ts, v0;
	uint32_t bad = 0;
	uint8_t *pre = NULL;
	int rc, nuq = 0, idx, prelen = 0;
	if (n < 0) n = 0;
	if (n > XS_VMAX) n = XS_VMAX;
	memset(&fl, 0, sizeof fl);
	fl.tid = tableid;
	fl.rid = rowid;
	fl.rec = deleted ? NULL : (const uint8_t *)blob;
	fl.reclen = deleted ? 0 : n;
	if (!deleted && blob != NULL && XS_IDX_ANY() &&
	    tableid != XS_CAT_TABLEID && tableid < XS_IDXCAT_TABLEID)
		nuq = xs_uniq_collect(bt, NULL, tableid, uq, 16);
	memset(&probe, 0, sizeof probe);
	probe.tableid = tableid; probe.rowid = rowid; probe.base = base;
	v0 = xs_visible();             /* before the probe: see g_row_cts */
	if (base != 0 && (rc = xs_ww_probe(bt, &probe, 1, &bad)) != 0)
		return rc > 0 ? SX_BUSY : rc;   /* rc < 0: unread */
	xs_commit_lock();
	xs_gate_wait_locked();
	if (insert && (rc = xs_key_live_locked(bt, tableid, rowid)) != 0) {
		xs_commit_unlock();
		return rc > 0 ? SX_CONSTRAINT : rc;   /* rc < 0: unread */
	}
	/* UNIQUE indexes: the check and the in-flight registration are one
	 * step, so two concurrent writers of one key cannot both pass. */
	rc = 0;
	if (nuq > 0)
		rc = xs_uniq_validate_locked(bt, NULL, tableid, rowid,
		    (const uint8_t *)blob, n, uq, nuq, &bad);
	if (rc != 0) {
		xs_commit_unlock();
		if (ubad != NULL) *ubad = bad;
		/* rc < 0: the probe's error */
		return rc > 0 ? SX_CONSTRAINT : rc;
	}
	if (base != 0 && (rc = xs_ww_recheck_locked(bt, &probe, 1, v0,
	    &bad)) != 0) {
		xs_commit_unlock();
		return rc > 0 ? SX_BUSY : rc;
	}
	/* From registration to removal the commit is masked: its entry lives
	 * on this stack, so a kill in between would leave g_inflight pointing
	 * into a dead fiber and pin the read horizon (test_wal_kill_engine). */
	(void)xtc_mask_enter();
	xs_inflight_add_locked(&fl);
	xs_uniq_stamp_locked(NULL, tableid, deleted ? NULL : (const uint8_t *)blob,
	    n, rowid, uq, nuq, fl.ts);
	xs_commit_unlock();
	ts = fl.ts;
	*out_ts = ts;
	if (g_xwal != NULL) {
		uint64_t lsn = 0;
		/* Durable BEFORE apply.  A failed log write applies nothing: the
		 * caller gets an error and the row never becomes visible. */
		if (xs_wal_put(ts, tableid, rowid, blob, n, deleted, &lsn) != XTC_OK) {
			xs_inflight_remove(&fl);
			xs_inflight_unlinked(&fl);
			(void)xtc_mask_leave();
			return SX_ERROR;
		}
		bt_set_lsn(bt, lsn);   /* stamp this change's LSN onto the page */
	}
	/* The pre-image for index maintenance: read while registered, so no
	 * other writer of the row can interleave (first-committer-wins). */
	idx = XS_IDX_ANY() && tableid != XS_CAT_TABLEID &&
	    tableid < XS_IDXCAT_TABLEID;
	if (idx && (pre = malloc(XS_VMAX)) != NULL)
		/* An unread pre-image taints the indexes (a rebuild follows). */
		prelen = xs_idx_preimage(bt, tableid, rowid, pre, /* RDERR_OK: taints */
		    XS_VMAX);
	enc_vkey(tableid, rowid, ts, key);
	buf[0] = deleted ? XS_F_DELETED : 0;
	if (!deleted && n > 0) memcpy(buf + 1, blob, (size_t)n);
	rc = bt_insert(bt, key, XS_VKLEN, buf, (uint16_t)(1 + (deleted ? 0 : n)))
	    == XTC_OK ? SX_OK : SX_ERROR;
	/* Index entries go in BEFORE the commit leaves the in-flight set: a
	 * concurrent UNIQUE check always sees one or the other. */
	if (rc == SX_OK && idx)
		xs_idx_maintain_ts(bt, tableid, rowid, (const uint8_t *)blob,
		    deleted ? 0 : n, deleted, prelen > 0 ? pre : NULL, prelen, ts);
	free(pre);
	xs_inflight_remove(&fl);
	/* fl is on this stack: return only once it is unlinked.  A caller
	 * that waits for the commit's visibility (out_vis) waits here, which
	 * implies it (the horizon never passes a linked entry). */
	if (vis && rc == SX_OK)
		xs_wait_visible(ts);
	else
		xs_inflight_unlinked(&fl);
	(void)xtc_mask_leave();      /* may not return: a latched kill */
	if (idx) xs_idx_reap(bt);
	return rc;
}

/* Has `rowid` a committed version newer than `base`?  1 if so, 0 if
 * not (or it has no version), or the XTC_E_* code of a read that
 * failed: nothing is then known, and a write-write or serializable
 * check that took it for "no" would let an update be lost.  Used by
 * those checks to detect a concurrent write to a key this transaction
 * read or writes. */
static int
xs_newer_than(bt_t *bt, uint32_t tableid, int64_t rowid, uint64_t base)
{
	bt_cursor_t *cur = NULL;
	uint8_t startk[XS_VKLEN];
	const void *k = NULL, *vv = NULL;
	uint16_t klen = 0, vl = 0;
	uint64_t ts = 0;
	int rc;
	enc_vkey(tableid, rowid, ~(uint64_t)0, startk);   /* newest version sorts first */
	rc = bt_cursor_open(bt, startk, XS_VKLEN, &cur);
	if (rc == XTC_OK) {
		rc = bt_cursor_next(cur, &k, &klen, &vv, &vl);
		if (rc == XTC_OK && xs_row_fault())
			rc = XTC_E_IO;
		if (rc == XTC_OK && klen == XS_VKLEN &&
		    dec_tableid((const uint8_t *)k) == tableid &&
		    dec_rowid((const uint8_t *)k) == rowid)
			ts = dec_ts((const uint8_t *)k);
		bt_cursor_close(cur);
	}
	if (rc != XTC_OK && rc != XTC_E_NOTFOUND) {
		(void)xs_rd_count(rc);
		/* planted 84: the unread row has no newer version */
		return XTC_DST_BUG(XTC_DST_BUG_WWREAD) ? 0 : rc;
	}
	return ts > base;
}

/*
 * The ids of dropped tables, collected by a pass over the tree (version
 * GC, the log dump) from the table catalog, which sorts before every
 * table: an id whose catalog row's newest version (the one the pass
 * keeps) is a tombstone.  The pass then drops that table's rows, which
 * DROP TABLE leaves behind.  Only a tombstoned id counts, never an id
 * with no catalog row: a caller may write rows under an id it never put
 * in the catalog.  If the set cannot grow, the rows stay (a leak, never
 * a loss).
 */
struct xs_dropset {
	uint32_t *id;      /* ascending: the catalog is scanned in order */
	int       n, cap;
};

static uint32_t xs_hw_parse(const uint8_t *p, int n);   /* defined below */

/*
 * The high-water mark a catalog version (key value `vv`, `vl` bytes,
 * flag byte first) records, or 0 for a tombstone.  A pass that drops
 * tombstoned ids reads the mark from rowid 0, which sorts before every
 * catalog row, and drops an id only if the mark covers it: a tombstone
 * above the mark (a DROP by a release without the mark) is kept, so its
 * id still counts as handed out and its rows stay.
 */
static uint32_t
xs_hw_of(const void *vv, uint16_t vl)
{
	const uint8_t *v = vv;
	if (vl < 1 || (v[0] & XS_F_DELETED))
		return 0;
	return xs_hw_parse(v + 1, (int)vl - 1);
}

static void
xs_dropset_add(struct xs_dropset *d, uint32_t id)
{
	uint32_t *nv;
	if (d->n == d->cap) {
		int nc = d->cap ? d->cap * 2 : 64;
		if ((nv = realloc(d->id, (size_t)nc * sizeof *nv)) == NULL)
			return;
		d->id = nv;
		d->cap = nc;
	}
	d->id[d->n++] = id;
}

static int
xs_dropset_has(const struct xs_dropset *d, uint32_t id)
{
	int lo = 0, hi = d->n - 1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		if (d->id[mid] == id) return 1;
		if (d->id[mid] < id) lo = mid + 1;
		else hi = mid - 1;
	}
	return 0;
}

/*
 * Version GC (MVCC vacuum).  Reclaims versions no live snapshot can
 * reach: scanning in key order the cursor sees each rowid's versions
 * newest-first (commit_ts descending), so for each rowid we keep every
 * version newer than `horizon`, keep the first version at-or-before
 * `horizon` (the survivor that snapshots at the horizon still need),
 * and delete the rest.  A survivor that is a tombstone with nothing
 * newer is itself unreachable and is dropped, so a deleted row
 * eventually leaves no trace.  A table whose catalog row is such a
 * tombstone (DROP TABLE below the horizon) loses every row with it.
 *
 * Latches must not span the vtable/btree boundary, and bt_delete
 * wants the exclusive latch a live cursor would hold, so this collects
 * the dead keys under an open cursor, closes it, then deletes -- the
 * same discipline the xstore cursor uses.  Deletes run in bounded
 * batches so memory stays O(batch), not O(dead).  Concurrent inserts
 * are safe: a committer writes at a timestamp above the horizon, which
 * GC never touches.
 */
#define XS_GC_BATCH 512
static int g_gc_batch = XS_GC_BATCH;      /* xstore_gc_set_batch (tests) */

/* Tests: cap a GC batch at n keys (1..XS_GC_BATCH) so a small table
 * crosses batch boundaries; 0 restores the default. */
void
xstore_gc_set_batch(int n)
{
	g_gc_batch = (n >= 1 && n <= XS_GC_BATCH) ? n : XS_GC_BATCH;
}

static int
xs_gc(bt_t *bt, uint64_t horizon, int *out_reclaimed)
{
	uint8_t resume[XS_VKLEN];
	int have_resume = 0, reclaimed = 0, rc = XTC_OK;
	/* Per-row state outlives a batch: a batch that ends right after a
	 * row's tombstone must not restart that row's older versions as a
	 * new row (its survivor would come back, without its index entries). */
	int64_t prev_rowid = 0;
	uint32_t prev_tableid = 0;
	int in_group = 0, survivor_found = 0;
	struct xs_dropset dropped = { NULL, 0, 0 };
	/* The catalogs' high-water rows: [0] tables, [1] indexes. */
	uint32_t mark[2] = { 0, 0 };
	const uint8_t past = XS_IDX_LEAD + 1;

	for (;;) {
		bt_cursor_t *cur = NULL;
		uint8_t dead[XS_GC_BATCH][XS_VKLEN];
		int ndead = 0, i;
		const uint8_t *start = have_resume ? resume : NULL;

#if XTC_DST_BUG(XTC_DST_BUG_GCBATCH)
		in_group = 0;     /* planted: the row state restarts per batch */
#endif

		rc = bt_cursor_open(bt, start, have_resume ? XS_VKLEN : 0, &cur);
		if (rc == XTC_OK && xs_row_fault()) {
			bt_cursor_close(cur);
			rc = XTC_E_IO;
		}
		if (rc != XTC_OK) {
			(void)xs_rd_count(rc);   /* stop: the rest only leaks */
			break;
		}
		for (;;) {
			const void *k = NULL, *vv = NULL;
			uint16_t klen = 0, vl = 0;
			int64_t rid; uint64_t ts; int deleted, first_in_group;
			uint32_t tid;
			int is_cat;

			rc = xs_rd_count(bt_cursor_next(cur, &k, &klen, &vv, &vl));
			if (rc != XTC_OK)
				break;
			/* The tables end at the index namespace (index entries
			 * are not versions, and one can be 20 bytes long too).
			 * The index catalog sorts after it, so seek past the
			 * namespace, as xs_dump_tree does, rather than stop:
			 * otherwise GC reached the index catalog only when the
			 * store held no index entry at all. */
			if (klen >= 1 && ((const uint8_t *)k)[0] == XS_IDX_LEAD) {
				bt_cursor_close(cur);
				cur = NULL;
				if ((rc = xs_rd_count(bt_cursor_open(bt, &past, 1,
				    &cur))) != XTC_OK)
					break;
				continue;
			}
			if (klen != XS_VKLEN)
				break;
			/* Skip the resume key itself (already processed). */
			if (have_resume && memcmp(k, resume, XS_VKLEN) == 0)
				continue;
			tid = dec_tableid((const uint8_t *)k);
			rid = dec_rowid((const uint8_t *)k);
			ts = dec_ts((const uint8_t *)k);
			if (tid == XS_STAGE_TID)
				continue;          /* STEAL staging area: not GC's concern */
			is_cat = (tid == XS_CAT_TABLEID || tid == XS_IDXCAT_TABLEID);
			deleted = (vl >= 1 && (((const uint8_t *)vv)[0] & XS_F_DELETED));
			/* A logical row is (tableid, rowid): a group boundary is
			 * either changing. */
			first_in_group = (!in_group || rid != prev_rowid ||
			    tid != prev_tableid);
			if (first_in_group) {
				prev_rowid = rid; prev_tableid = tid;
				in_group = 1; survivor_found = 0;
				if (is_cat && rid == 0)
					mark[tid == XS_IDXCAT_TABLEID] = xs_hw_of(vv, vl);
				/* A table dropped below the horizon: no snapshot
				 * can reach it, so its rows go with its tombstone. */
				if (tid == XS_CAT_TABLEID && rid > 0 && deleted &&
				    ts <= horizon && (uint32_t)rid <= mark[0])
					xs_dropset_add(&dropped, (uint32_t)rid);
			}
			if (tid != XS_CAT_TABLEID && xs_dropset_has(&dropped, tid)) {
				memcpy(dead[ndead++], k, XS_VKLEN);
			} else if (!survivor_found) {
				if (ts <= horizon) {
					survivor_found = 1;
					/* An unmarked tombstone in either catalog
					 * stays: it alone keeps its id from being
					 * handed out again. */
					if (deleted && first_in_group &&
					    !(is_cat && rid > 0 && (uint32_t)rid >
					    mark[tid == XS_IDXCAT_TABLEID]))
						memcpy(dead[ndead++], k, XS_VKLEN);
				}
				/* else ts > horizon: keep (newer than horizon). */
			} else {
				memcpy(dead[ndead++], k, XS_VKLEN);   /* older than survivor */
			}
			if (ndead == g_gc_batch) {
				memcpy(resume, k, XS_VKLEN);
				have_resume = 1;
				break;
			}
		}
		if (cur != NULL)
			bt_cursor_close(cur);
		for (i = 0; i < ndead; i++) {
			if (bt_delete(bt, dead[i], XS_VKLEN) == XTC_OK) reclaimed++;
		}
		if (ndead < g_gc_batch)
			break;     /* scanned to the end, or stopped on a read */
	}
	free(dropped.id);
	if (out_reclaimed != NULL) *out_reclaimed = reclaimed;
	return rc == XTC_E_NOTFOUND ? XTC_OK : rc;
}

/*
 * Opportunistic inline prune of ONE rowid's dead versions, up to the
 * given horizon -- PostgreSQL's HOT-style pruning: triggered on the
 * write that creates a new version, it touches only this rowid's
 * version chain (a handful of entries, on pages already hot from the
 * write), so it is incremental and cache-friendly, unlike a full-tree
 * vacuum.  Same keep rule as xs_gc, restricted to one rowid; same
 * collect-then-delete discipline (no latch across the cursor).
 */
static int
xs_prune_rowid(bt_t *bt, uint32_t tableid, int64_t rowid, uint64_t horizon)
{
	bt_cursor_t *cur = NULL;
	uint8_t startk[XS_VKLEN];
	uint8_t dead[16][XS_VKLEN];
	int ndead = 0, i, survivor_found = 0, first = 1, reclaimed = 0;

	enc_vkey(tableid, rowid, ~(uint64_t)0, startk);    /* newest version first */
	/* A read error stops the prune (counted): versions leak, no more. */
	if (xs_rd_count(bt_cursor_open( /* RDERR_OK: a leak */
	    bt, startk, XS_VKLEN, &cur)) != XTC_OK)
		return 0;                 /* a read error stops it: a leak only */
	for (;;) {
		const void *k = NULL, *vv = NULL;
		uint16_t klen = 0, vl = 0;
		uint64_t ts; int deleted;

		if (xs_rd_count(bt_cursor_next( /* RDERR_OK: a leak */
		    cur, &k, &klen, &vv, &vl)) != XTC_OK ||
		    klen != XS_VKLEN ||
		    dec_tableid((const uint8_t *)k) != tableid ||
		    dec_rowid((const uint8_t *)k) != rowid)
			break;                            /* past this (table,rowid) */
		ts = dec_ts((const uint8_t *)k);
		deleted = (vl >= 1 && (((const uint8_t *)vv)[0] & XS_F_DELETED));
		if (!survivor_found) {
			if (ts <= horizon) {
				survivor_found = 1;
				if (deleted && first) memcpy(dead[ndead++], k, XS_VKLEN);
#if XTC_DST_BUG(XTC_DST_BUG_PRUNEHZN)
				/* planted: prune the horizon's own version, not
				 * only the ones below it */
				else if (!first) memcpy(dead[ndead++], k, XS_VKLEN);
#endif
			}
		} else {
			memcpy(dead[ndead++], k, XS_VKLEN);
		}
		first = 0;
		if (ndead == (int)(sizeof dead / sizeof dead[0])) break;
	}
	bt_cursor_close(cur);
	for (i = 0; i < ndead; i++)
		if (bt_delete(bt, dead[i], XS_VKLEN) == XTC_OK) reclaimed++;
	return reclaimed;
}

/*
 * Adaptive autovacuum gate.  Pruning every write is pure overhead on a
 * workload whose version chains are naturally short (each prune walks
 * the chain and finds nothing).  This self-tunes: prune every write
 * while prunes keep reclaiming versions (hot keys, long chains), but
 * back off geometrically -- pruning only 1 in `interval` writes -- when
 * a prune finds nothing, so a uniform workload pays almost no prune
 * cost.  Global (the engine is one shared store); a couple of relaxed
 * atomics, no per-rowid state. */
#define XS_AV_MAX_INTERVAL 256
static int g_av_interval = 1;
static int g_av_countdown = 1;
static uint64_t g_av_prunes = 0; /* prune passes actually run */
/* The horizon is computed only for a write that prunes: snap_horizon is
 * a seq_cst fence plus a load of each of the SNAP_MAX pin slots, and
 * every written row paid it before the countdown skipped the prune
 * (4.5% of a TPC-C connection's CPU at one connection). */
static void
xs_maybe_prune(bt_t *bt, uint32_t tableid, int64_t rowid)
{
	int reclaimed, iv;
	if (xtc_atomic_fetch_sub_i32(&g_av_countdown, 1, xtc_mo_relaxed) > 1)
		return;                               /* not this write */
	/* A count of versions reclaimed: an error reclaims 0. */
	reclaimed = xs_prune_rowid(bt, tableid, rowid, /* RDERR_OK: count */
	    snap_horizon(xs_visible()));
	xtc_atomic_fetch_add_u64(&g_av_prunes, 1, xtc_mo_relaxed);
	if (reclaimed > 0) {
		iv = 1;                               /* productive: prune every write */
	} else {
		iv = xtc_atomic_load_i32(&g_av_interval, xtc_mo_relaxed) * 2;
		if (iv > XS_AV_MAX_INTERVAL) iv = XS_AV_MAX_INTERVAL;
	}
	xtc_atomic_store_i32(&g_av_interval, iv, xtc_mo_relaxed);
	xtc_atomic_store_i32(&g_av_countdown, iv, xtc_mo_relaxed);
}

/* ---- row write intents ------------------------------------------- *
 *
 * First-UPDATER-wins at the statement, as PostgreSQL (row locks) and
 * SQLite (one writer) report it, instead of only first-COMMITTER-wins at
 * COMMIT.  A transaction's write first takes the row's intent: free ->
 * taken; held by this transaction -> fine; held by another -> wait (a
 * fiber parks, a plain thread naps) until the holder ends, then
 * re-check.  A row committed past the
 * snapshot the write was computed against fails the statement (a lost
 * update: waiting cannot fix it), and so does a wait-for cycle (the
 * youngest transaction in it is the one that fails) or a wait past the
 * cap.  The commit-time first-committer-wins check is unchanged, so
 * correctness never depends on the intents; they only move the failure
 * to the statement and make a second writer wait instead of doing work
 * that is certain to be thrown away.
 *
 * An autocommit UPDATE/DELETE takes the row's intent for the length of
 * its put (so it waits for a transaction holding the row, and a
 * transaction waits for it).  A fresh INSERT takes none: it has no read
 * to go stale, and the UNIQUE / commit checks refuse a duplicate key.
 *
 * The table is a fixed open-addressed hash under striped plain mutexes
 * (leaf locks: held for a few instructions, never across a park).
 * Owners are slots in g_ilk_owner, static and never freed, so a waiter
 * can follow a holder's wait-for edge even after the holder's connection
 * closed: (slot, seq) names one transaction and a stale seq reads as
 * free.  Waits nap with xtc_proc_sleep and the cap counts nap time, so
 * no clock or RNG is read (deterministic under the simulator).
 */
#define XS_ILK_STRIPE 256u
#define XS_ILK_PER    256                 /* intents per stripe (64k in all) */
#define XS_ILK_OWNERS 1024
#define XS_ILK_TXN_MAX 4096               /* intents one transaction takes */
#define XS_ILK_NAP0_NS (20LL * 1000)
#define XS_ILK_NAPMAX_NS (1000LL * 1000)
#define XS_ILK_WAIT_NS (10LL * 1000 * 1000 * 1000)   /* give up: conflict */

struct xs_ilk_ent { const bt_t *bt; uint32_t tid; int64_t rid; int slot; uint64_t seq; };
/* One stripe: a mutex and a compact array of held intents (a take scans
 * the stripe's few entries; a release swap-removes). */
static struct xs_ilk_stripe {
	pthread_mutex_t   mu;
	int               n;
	struct xs_ilk_ent ent[XS_ILK_PER];
} g_ilk[XS_ILK_STRIPE];
static pthread_once_t g_ilk_once = PTHREAD_ONCE_INIT;
static struct xs_ilk_owner {
	uint64_t seq;  /* current txn of this slot (0 = idle) */
	int wait_slot; /* the slot it waits for, or 0 */
	uint64_t wait_seq;
} g_ilk_owner[XS_ILK_OWNERS + 1];
static int g_ilk_next_slot = 1;
static int g_ilk_free[XS_ILK_OWNERS], g_ilk_nfree;   /* recycled slots */
static pthread_mutex_t g_ilk_free_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_ilk_seqgen = 1;
static uint64_t g_ilk_waits, g_ilk_conflicts, g_ilk_deadlocks;
static uint64_t g_ilk_wait_ns; /* nap time spent waiting (metric) */

static void
xs_ilk_init(void)
{
	unsigned i;
	for (i = 0; i < XS_ILK_STRIPE; i++)
		(void)pthread_mutex_init(&g_ilk[i].mu, NULL);  /* leaf lock, never held across a park */
}

/* The key's stripe.  Not hashed on the bt address (ASLR would make a
 * full stripe, and so a run, differ between replays of one seed). */
static struct xs_ilk_stripe *
xs_ilk_stripe(const bt_t *bt, uint32_t tid, int64_t rid)
{
	uint64_t x = ((uint64_t)tid << 40) ^ (uint64_t)rid;
	(void)bt;
	x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
	(void)pthread_once(&g_ilk_once, xs_ilk_init);
	return &g_ilk[x % XS_ILK_STRIPE];
}

/* Is (slot, seq) still the live transaction of its owner slot? */
static int
xs_ilk_live(int slot, uint64_t seq)
{
	return slot > 0 && slot <= XS_ILK_OWNERS && seq != 0 &&
	    xtc_atomic_load_u64(&g_ilk_owner[slot].seq, xtc_mo_acquire) == seq;
}

/* Find or take (bt, tid, rid) for (slot, seq); slot 0 = only look.
 * Returns 0 taken, 2 already ours, 1 held by another live transaction
 * (*hslot / *hseq set), 3 free and not taken (look only), -1 no room. */
static int
xs_ilk_try(const bt_t *bt, uint32_t tid, int64_t rid, int slot, uint64_t seq,
    int *hslot, uint64_t *hseq)
{
	struct xs_ilk_stripe *sp = xs_ilk_stripe(bt, tid, rid);
	struct xs_ilk_ent *e = NULL;
	int i, rc = 0;
	(void)pthread_mutex_lock(&sp->mu);   /* leaf lock */
	for (i = 0; i < sp->n; i++)
		if (sp->ent[i].rid == rid && sp->ent[i].tid == tid &&
		    sp->ent[i].bt == bt) {
			e = &sp->ent[i];
			break;
		}
	if (e != NULL && xs_ilk_live(e->slot, e->seq)) {
		if (e->slot == slot && e->seq == seq) rc = 2;
		else { *hslot = e->slot; *hseq = e->seq; rc = 1; }
		goto out;
	}
	if (slot == 0) { rc = 3; goto out; }
	/* Free, or left by a transaction that ended: (re)take it.  A full
	 * stripe first reuses an entry whose transaction ended. */
	for (i = 0; e == NULL && sp->n == XS_ILK_PER && i < sp->n; i++)
		if (!xs_ilk_live(sp->ent[i].slot, sp->ent[i].seq))
			e = &sp->ent[i];
	if (e == NULL && sp->n == XS_ILK_PER) { rc = -1; goto out; }
	if (e == NULL) e = &sp->ent[sp->n++];
	e->bt = bt; e->tid = tid; e->rid = rid; e->slot = slot; e->seq = seq;
out:
	(void)pthread_mutex_unlock(&sp->mu);   /* leaf lock */
	return rc;
}

static void
xs_ilk_drop(const bt_t *bt, uint32_t tid, int64_t rid, int slot, uint64_t seq)
{
	struct xs_ilk_stripe *sp = xs_ilk_stripe(bt, tid, rid);
	int i;
	(void)pthread_mutex_lock(&sp->mu);   /* leaf lock */
	for (i = 0; i < sp->n; i++) {
		struct xs_ilk_ent *e = &sp->ent[i];
		if (e->rid == rid && e->tid == tid && e->bt == bt) {
			if (e->slot == slot && e->seq == seq)
				*e = sp->ent[--sp->n];
			break;
		}
	}
	(void)pthread_mutex_unlock(&sp->mu);   /* leaf lock */
}

/* Waiting for (hslot, hseq): does the wait-for chain lead back to us,
 * and is this transaction the cycle's victim?  The victim is the
 * youngest transaction (largest seq) in the cycle; every waiter in it
 * re-walks after each nap, so exactly one fails (the walk publishes
 * nothing; the caller published its own edge first, so of two
 * transactions closing a cycle at once at least one sees it). */
static int
xs_ilk_victim(int slot, uint64_t seq, int hslot, uint64_t hseq)
{
	uint64_t youngest = seq;
	int depth;
	for (depth = 0; depth < 64; depth++) {
		int ns;
		uint64_t nq;
		if (hslot == slot)   /* back to us (a stale edge names an old seq) */
			return hseq == seq && youngest == seq;
		if (!xs_ilk_live(hslot, hseq)) return 0;
		if (hseq > youngest) youngest = hseq;
		ns = xtc_atomic_load_i32(&g_ilk_owner[hslot].wait_slot,
		    xtc_mo_seq_cst);
		nq = xtc_atomic_load_u64(&g_ilk_owner[hslot].wait_seq,
		    xtc_mo_seq_cst);
		if (ns == 0) return 0;
		hslot = ns; hseq = nq;
	}
	return 0;   /* a cycle not through us (or a huge one: the cap ends it) */
}

/* This transaction's identity for intents (allocated on first use).
 * Returns 0 if the process ran out of owner slots (no intents). */
static int
xs_ilk_self(xstore_ctx_t *cx)
{
	if (cx->ilk_slot == 0) {
		int s = 0;
		(void)pthread_mutex_lock(&g_ilk_free_mu);    /* leaf lock */
		if (g_ilk_nfree > 0) s = g_ilk_free[--g_ilk_nfree];
		(void)pthread_mutex_unlock(&g_ilk_free_mu);  /* leaf lock */
		if (s == 0)
			s = xtc_atomic_fetch_add_i32(&g_ilk_next_slot, 1,
			    xtc_mo_relaxed);
		/* shortcut: > 1024 connections at once: the extra ones take no
		 * intents (their conflicts surface at COMMIT, as before). */
		if (s > XS_ILK_OWNERS) { cx->ilk_slot = -1; return 0; }
		cx->ilk_slot = s;
	}
	if (cx->ilk_slot < 0) return 0;
	if (cx->ilk_seq == 0) {
		cx->ilk_seq =
		    xtc_atomic_fetch_add_u64(&g_ilk_seqgen, 1, xtc_mo_relaxed);
		xtc_atomic_store_u64(&g_ilk_owner[cx->ilk_slot].seq,
		    cx->ilk_seq, xtc_mo_release);
	}
	return 1;
}

static void
xs_ilk_wait_clear(xstore_ctx_t *cx)
{
	xtc_atomic_store_i32(&g_ilk_owner[cx->ilk_slot].wait_slot, 0,
	    xtc_mo_release);
}

/*
 * A transaction's write of (tid, rid): take the row's write intent,
 * waiting for another holder to finish, and check that no version was
 * committed past `base` (the snapshot the write was computed against).
 * Returns 0, or the conflict (XS_CONFL_STALE: a newer committed
 * version; XS_CONFL_HARD: a deadlock, or a wait past the cap), or -1
 * when the row could not be read (rd_err says why).
 */
static int xs_stmt_restartable(const xstore_ctx_t *cx);

static int
xs_intent_take(xstore_ctx_t *cx, uint32_t tid, int64_t rid, uint64_t base)
{
	int hslot = 0, r, nr, dl = 0;
	uint64_t hseq = 0;
	int64_t waited = 0, nap = XS_ILK_NAP0_NS;
	if (tid == XS_CAT_TABLEID || tid >= XS_IDXCAT_TABLEID) return 0;
	/* shortcut: a transaction past XS_ILK_TXN_MAX rows takes no more
	 * intents (bounds its cost and the table); its later rows are
	 * checked at COMMIT only.  Upgrade: a table-level intent. */
	if (cx->ilk_n >= XS_ILK_TXN_MAX) return 0;
	if (!xs_ilk_self(cx)) return 0;
	for (;;) {
		r = xs_ilk_try(cx->bt, tid, rid, cx->ilk_slot, cx->ilk_seq, &hslot, &hseq);
		if (r == 2) return 0;              /* ours: checked when taken */
		/* Committed past our read: a lost update, waiting cannot help
		 * -- unless the statement will re-run on a fresh snapshot.
		 * Then wait for the holder FIRST (r == 1): its version is in the
		 * tree before it is visible, so checking now found it "past"
		 * the snapshot the re-run had just taken, and the statement
		 * burned its 8 re-runs without waiting once (TPC-C Payment's
		 * warehouse UPDATE: 1.35M retries in 60 s at 32 threads). */
		nr = 0;
		if (base != 0 && !(r == 1 && xs_stmt_restartable(cx)))
			nr = xs_newer_than(cx->bt, tid, rid, base);
		if (nr < 0) {                      /* unread: unknown */
			if (r == 0) xs_ilk_drop(cx->bt, tid, rid, cx->ilk_slot, cx->ilk_seq);
			xs_ilk_wait_clear(cx);
			xs_rd_err_tid(cx, tid, nr);
			return -1;
		}
		if (nr > 0) {
			if (r == 0) xs_ilk_drop(cx->bt, tid, rid, cx->ilk_slot, cx->ilk_seq);
			xs_ilk_wait_clear(cx);
			xtc_atomic_fetch_add_u64(&g_ilk_conflicts, 1,
			    xtc_mo_relaxed);
			xtc_atomic_fetch_add_u64(&g_ilk_wait_ns,
			    (uint64_t)waited, xtc_mo_relaxed);
			return XS_CONFL_STALE;
		}
		if (r == 0 || r < 0) break;        /* taken; or no room: COMMIT checks */
		/* Held by another transaction.  Publish the edge, then look for
		 * a cycle through it. */
		xtc_atomic_store_u64(&g_ilk_owner[cx->ilk_slot].wait_seq, hseq,
		    xtc_mo_seq_cst);
		xtc_atomic_store_i32(&g_ilk_owner[cx->ilk_slot].wait_slot,
		    hslot, xtc_mo_seq_cst);
		/* Off a loop (an embedded caller on a plain thread) the wait
		 * is a thread nap on the same schedule, not an immediate
		 * conflict: that made a retrying client spin (2.8M Payment
		 * retries in 60 s at 32 threads) where a fiber would wait. */
		if (waited >= XS_ILK_WAIT_NS ||
		    (dl = xs_ilk_victim(cx->ilk_slot, cx->ilk_seq, hslot, hseq))) {
			if (dl)
				xtc_atomic_fetch_add_u64(&g_ilk_deadlocks, 1,
				    xtc_mo_relaxed);
			xs_ilk_wait_clear(cx);
			xtc_atomic_fetch_add_u64(&g_ilk_conflicts, 1,
			    xtc_mo_relaxed);
			xtc_atomic_fetch_add_u64(&g_ilk_wait_ns,
			    (uint64_t)waited, xtc_mo_relaxed);
			return XS_CONFL_HARD;
		}
		if (waited == 0)
			xtc_atomic_fetch_add_u64(&g_ilk_waits, 1,
			    xtc_mo_relaxed);
		if (xtc_pid_is_none(xtc_self()))
			(void)xtc_thrd_sleep_ns(nap);
		else
			(void)xtc_proc_sleep(nap);
		waited += nap;
		if (nap < XS_ILK_NAPMAX_NS) nap *= 2;
	}
	xs_ilk_wait_clear(cx);
	if (waited > 0)
		xtc_atomic_fetch_add_u64(&g_ilk_wait_ns, (uint64_t)waited,
		    xtc_mo_relaxed);
	if (r < 0) return 0;
	if (cx->ilk_n == cx->ilk_cap) {
		int nc = cx->ilk_cap ? cx->ilk_cap * 2 : 16;
		struct xs_ikey *nk = realloc(cx->ilk, sizeof *nk * (size_t)nc);
		if (nk == NULL) {   /* cannot remember it: give it back, COMMIT checks */
			xs_ilk_drop(cx->bt, tid, rid, cx->ilk_slot, cx->ilk_seq);
			return 0;
		}
		cx->ilk = nk; cx->ilk_cap = nc;
	}
	cx->ilk[cx->ilk_n].tid = tid;
	cx->ilk[cx->ilk_n++].rid = rid;
	return 0;
}

/* Would a stale conflict on this transaction statement re-run it on a
 * fresh snapshot?  The in-transaction cases of xstore_stmt_restart. */
static int
xs_stmt_restartable(const xstore_ctx_t *cx)
{
	if (!cx->in_txn) return 0;
	if (cx->txn_iso < XS_ISO_REPEATABLE_READ) return 1;
	return cx->stmt_first && !cx->serializable && cx->wn == 0;
}

/* Release every intent of cx's transaction (it committed -- its versions
 * are in the tree -- or rolled back).  Ending the owner's seq frees them
 * all at once for every prober; the entries are cleared too so the table
 * does not fill. */
static void
xs_intents_release(xstore_ctx_t *c)
{
	int i;
	if (c->ilk_slot <= 0 || c->ilk_seq == 0) return;
	xtc_atomic_store_u64(&g_ilk_owner[c->ilk_slot].seq, 0, xtc_mo_release);
	for (i = 0; i < c->ilk_n; i++)
		xs_ilk_drop(c->bt, c->ilk[i].tid, c->ilk[i].rid, c->ilk_slot, c->ilk_seq);
	c->ilk_n = 0;
	c->ilk_seq = 0;
}

/* A closing connection returns its owner slot.  Safe to reuse at once: a
 * waiter that still names the old (slot, seq) sees a different seq. */
static void
xs_ilk_slot_free(xstore_ctx_t *c)
{
	if (c->ilk_slot <= 0) return;
	xs_intents_release(c);
	xs_ilk_wait_clear(c);
	(void)pthread_mutex_lock(&g_ilk_free_mu);    /* leaf lock */
	if (g_ilk_nfree < XS_ILK_OWNERS) g_ilk_free[g_ilk_nfree++] = c->ilk_slot;
	(void)pthread_mutex_unlock(&g_ilk_free_mu);  /* leaf lock */
	c->ilk_slot = 0;
}

/* Engine reset (log attach/detach, recovery; no transaction can be in
 * flight): forget every intent, so one held by a proc that was killed
 * mid-transaction (its ctx never freed) cannot block forever. */
static void
xs_ilk_reset(void)
{
	unsigned i;
	for (i = 1; i <= XS_ILK_OWNERS; i++) {
		xtc_atomic_store_u64(&g_ilk_owner[i].seq, 0, xtc_mo_release);
		xtc_atomic_store_i32(&g_ilk_owner[i].wait_slot, 0,
		    xtc_mo_release);
	}
	(void)pthread_once(&g_ilk_once, xs_ilk_init);
	for (i = 0; i < XS_ILK_STRIPE; i++) {
		(void)pthread_mutex_lock(&g_ilk[i].mu);    /* leaf lock */
		g_ilk[i].n = 0;
		(void)pthread_mutex_unlock(&g_ilk[i].mu);  /* leaf lock */
	}
}

uint64_t
xstore_intent_waits(void)
{
	return xtc_atomic_load_u64(&g_ilk_waits, xtc_mo_relaxed);
}

uint64_t
xstore_intent_conflicts(void)
{
	return xtc_atomic_load_u64(&g_ilk_conflicts, xtc_mo_relaxed);
}

uint64_t
xstore_intent_wait_ns(void)
{
	return xtc_atomic_load_u64(&g_ilk_wait_ns, xtc_mo_relaxed);
}

uint64_t
xstore_intent_deadlocks(void)
{
	return xtc_atomic_load_u64(&g_ilk_deadlocks, xtc_mo_relaxed);
}

/* Write a version, then (if this connection enabled autovacuum) prune
 * the rowid's now-dead older versions up to the live-snapshot horizon.
 * Also maintains any secondary indexes on the table: the pre-image is
 * captured before the write (to remove its old index entry) and the new
 * value's entry is added after. */
#ifdef SQLXTC_TEST_PLANT
/* A planted bug for the io soak's negative control, compiled only into
 * sqlxtc-server-plant (make sqlxtc-server-plant), never into the server:
 * SQLXTC_PLANT_DROP=N reports every Nth autocommit row write applied
 * without writing it (a lost acknowledged write).
 * test/sqlxtc/test_sqlxtc_iosoak.sh must fail on it. */
static unsigned long g_plant_drop_n;
static int
xs_plant_drop(void)
{
	const char *e = getenv("SQLXTC_PLANT_DROP");
	unsigned long every = e != NULL ? strtoul(e, NULL, 10) : 0;
	return every > 0 &&
	    (xtc_atomic_fetch_add_ulong(&g_plant_drop_n, 1, xtc_mo_seq_cst) +
	        1) %
	        every ==
	    0;
}
#endif

static int
xs_put_pruned(xstore_ctx_t *cx, uint32_t tableid, int64_t rowid, const void *blob, int n,
    int deleted, int insert)
{
	uint32_t ubad = 0;
	int rc;
#ifdef SQLXTC_TEST_PLANT
	if (!deleted && xs_plant_drop())
		return SX_OK;
#endif
	rc = xs_put_ts(cx->bt, tableid, rowid, blob, n, deleted, cx->stmt_base,
	    insert, &cx->last_ts, &ubad, 1);
	if (rc == SX_CONSTRAINT && ubad != 0) {
		struct xs_idx_ent e;
		if (xs_idx_get(cx->bt, ubad, &e))
			xs_uniq_msg(&e, cx->uniq_err, sizeof cx->uniq_err);
	} else if (rc < 0) {
		if (ubad != 0)
			xs_idx_err_msg(cx->bt, ubad, rc, cx->uniq_err,
			    sizeof cx->uniq_err);
		else
			xs_rd_err_tid(cx, tableid, rc);
		rc = SX_ERROR;
	}
	if (rc == SX_OK) {
		cx->stmt_puts++;            /* an applied autocommit row */
		if (cx->autovacuum)
			xs_maybe_prune(cx->bt, tableid, rowid);
	}
	return rc;
}

/* Buffer a write in an open transaction, spilling older buffered
 * payloads to the STEAL staging area once they exceed the high-water
 * mark, so a large transaction does not grow its resident footprint
 * without bound during execution. */
static int
xs_buf_write(xstore_ctx_t *cx, uint32_t tableid, int64_t rowid,
    const void *blob, int n, int deleted)
{
	int rc = wbuf_add(cx, tableid, rowid, blob, n, deleted);
	if (rc == SX_OK && cx->wbuf_bytes > XS_SPILL_HI)
		xs_spill_payloads(cx);
	return rc;
}

/*
 * Native write entry points (transaction-aware).  These are what the
 * VDBE-free write path (vexec) calls: they route a write the SAME way
 * xs_update does -- buffer into the connection's wbuf when a transaction
 * is open (so COMMIT flushes it atomically at one timestamp, ROLLBACK
 * discards it, and it commits together with any VDBE writes in the same
 * BEGIN..COMMIT), or autocommit immediately (one fresh timestamp) in
 * autocommit mode.  Transaction open/close is driven by SQLite's
 * autocommit flag exactly as the vtab path is, via xs_enter_ctx, so the
 * two paths share one transaction.  Returns 0 on success, <0 on error.
 */
int
xstore_write_txn(struct xsql *db, uint32_t tableid, int64_t rowid,
                 const uint8_t *rec, int reclen, int deleted)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	int rc;
	if (cx == NULL) return -1;
	xs_enter_ctx(cx, db);   /* sync in_txn with SQLite's autocommit flag */
	cx->uniq_err[0] = '\0';
	cx->last_conflict = 0;
	if (cx->in_txn) {
		cx->touched = 1;
		/* The row's write intent: wait for a concurrent writer of the
		 * row; a version committed past our snapshot fails the
		 * statement (not, later, the COMMIT). */
		if (!cx->ilk_skip && (rc = xs_intent_take(cx, tableid, rowid,
		    cx->txn_iso >= XS_ISO_REPEATABLE_READ ? cx->txn_snap
		    : cx->stmt_base)) != 0) {
			if (rc > 0) cx->last_conflict = rc;
			return -1;
		}
		/* UNIQUE: against committed rows and this txn's own writes now;
		 * the commit re-validates against concurrent committers. */
		rc = 0;
		if (!deleted)
			rc =
			    xs_uniq_check_stmt(cx, tableid, rowid, rec, reclen);
		if (rc != 0)
			return rc > 0 ? 1 : -1; /* -1: uniq_err says why */
		rc = xs_buf_write(cx, tableid, rowid, deleted ? NULL : rec,
		                  deleted ? 0 : reclen, deleted);
		return rc == SX_OK ? 0 : -1;
	}
	/* Autocommit (a one-row statement; a multi-row one runs in an
	 * implicit transaction): an UPDATE/DELETE computed this row from its
	 * read at stmt_base; xs_put_ts refuses it (SX_BUSY) if the row was
	 * committed past that since, or a commit writing it is in flight.
	 * Holding the row's intent for the put makes it wait for a
	 * transaction that holds the row (and that transaction wait for
	 * it). */
	/* base 0: xs_put_ts makes the stale-read check under the commit lock */
	if ((rc = xs_intent_take(cx, tableid, rowid, 0)) != 0) {
		xs_intents_release(cx);
		if (rc > 0) cx->last_conflict = rc;
		return -1;
	}
	/* A read error the statement made, or a kill pending once the row
	 * is ours (the wait above is the last park), discards a one-row
	 * statement, as xstore_stmt_implicit_end discards a multi-row one. */
	if (xs_stmt_doomed(cx)) {
		xs_intents_release(cx);
		return -1;
	}
	rc = xs_put_pruned(cx, tableid, rowid, deleted ? NULL : rec,
	                   deleted ? 0 : reclen, deleted, 0);
	if (rc == SX_BUSY)
		cx->last_conflict = XS_CONFL_STALE;
	if (rc == SX_OK) {
		xs_wait_visible(cx->last_ts);
	}
	/* Released once the version is visible: a waiter that re-runs on a
	 * new snapshot then sees it (as a transaction releases in
	 * wbuf_clear, after its commit's xs_wait_visible). */
	xs_intents_release(cx);
	if (rc == SX_CONSTRAINT && cx->uniq_err[0] != '\0')
		return 1;
	return rc == SX_OK ? 0 : -1;
}

/* INSERT of one new row: like xstore_write_txn, but the key must not
 * exist.  Autocommit checks that atomically with the write (see
 * xs_put_ts); in a transaction (a multi-row statement's implicit one
 * too) the statement checked it and the commit's write-write check
 * catches a row committed since the snapshot.
 * Returns 0, 1 if the key exists (unique violation), or -1 on error. */
int
xstore_insert_txn(struct xsql *db, uint32_t tableid, int64_t rowid,
                  const uint8_t *rec, int reclen)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	int rc;
	if (cx == NULL) return -1;
	xs_enter_ctx(cx, db);
	if (cx->in_txn) {
		/* A new row: no snapshot read to go stale, and a concurrent
		 * insert of the same key is refused by the commit (and UNIQUE)
		 * checks -- no intent (a bulk load would fill the table). */
		cx->ilk_skip = 1;
		rc = xstore_write_txn(db, tableid, rowid, rec, reclen, 0);
		cx->ilk_skip = 0;
		return rc;
	}
	if (xs_stmt_doomed(cx))
		return -1;            /* as in xstore_write_txn */
	cx->uniq_err[0] = '\0';
	rc = xs_put_pruned(cx, tableid, rowid, rec, reclen, 0, 1);
	if (rc == SX_OK) {
		xs_wait_visible(cx->last_ts);
		return 0;
	}
	return rc == SX_CONSTRAINT ? 1 : -1;
}

/* The message of the connection's last UNIQUE violation ("UNIQUE
 * constraint failed: t.a, t.b"), or "" if the last refused write was a
 * rowid-key conflict (the caller formats that one). */
const char *
xstore_uniq_err(struct xsql *db)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	return cx != NULL ? cx->uniq_err : "";
}

/* The current statement's first read error ("index error: cannot read
 * index t_g on t: ...", "cannot read table t: ...", "cannot read the
 * catalog: ..."), or "" if none. */
const char *
xstore_read_err(struct xsql *db)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	char name[64];
	int i;
	if (cx == NULL)
		return "";
	if (cx->rd_err[0] == '\0' && cx->rd_rc != 0 &&
	    cx->rd_tid == XS_CAT_TABLEID) {
		snprintf(cx->rd_err, sizeof cx->rd_err,
		    "cannot read the catalog: %s", xtc_strerror(cx->rd_rc));
	} else if (cx->rd_err[0] == '\0' && cx->rd_rc != 0) {
		snprintf(name, sizeof name, "id %u", (unsigned)cx->rd_tid);
		xs_cat_lock();
		for (i = 0; i < g_cat_n; i++)
			if (g_cat[i].bt == cx->bt && g_cat[i].tableid == cx->rd_tid) {
				snprintf(name, sizeof name, "%s", g_cat[i].name);
				break;
			}
		xs_cat_unlock();
		/* Out of memory is not a failed read: say so. */
		snprintf(cx->rd_err, sizeof cx->rd_err,
		    cx->rd_rc == XTC_E_NOMEM ? "cannot load table %s: %s" :
		    "cannot read table %s: %s", name, xtc_strerror(cx->rd_rc));
	}
	return cx->rd_err;
}

/* Record a failed catalog read as the statement's read error (see
 * xstore.h). */
void
xstore_read_err_catalog(struct xsql *db, int rc)
{
	xs_rd_err_tid(xstore_ctx_of(db), XS_CAT_TABLEID, rc);
}

/* Clear the connection's read error (see xstore.h). */
void
xstore_read_err_clear(struct xsql *db)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	if (cx != NULL)
		xs_rd_err_clear(cx);
}

/* Statement atomicity inside a transaction: a failed statement undoes
 * its own buffered writes and leaves the transaction open (SQLite).
 * stmt_mark returns the buffer position to undo to (-1 outside a txn). */
int
xstore_stmt_mark(struct xsql *db)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	if (cx == NULL) return -1;
	xs_enter_ctx(cx, db);
	return cx->in_txn ? cx->wn : -1;
}

void
xstore_stmt_undo(struct xsql *db, int mark)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	if (cx == NULL || mark < 0 || !cx->in_txn || mark > cx->wn) return;
	if (mark < cx->wn)
		wbuf_truncate(cx, mark);
}

/*
 * Auto-assigned rowids.  The highest rowid handed out per (bt, table) in
 * this process, so concurrent INSERTs that omit the key never pick the
 * same one (each would otherwise compute the same committed max + 1).
 * A rolled-back reservation leaves a gap, as SQLite may.  Guarded by its
 * own leaf lock (it was the commit lock: every auto-rowid INSERT then
 * queued behind the committers -- 70 of 384 client stacks at 64 clients
 * waited there); entries for a bt are dropped when it closes.  If the table
 * is full the old max + 1 is used, and a collision is then refused by the
 * INSERT's uniqueness check rather than lost.
 */
#define XS_ROWID_HW_MAX 256
static struct xs_rowid_hw {
	bt_t    *bt;
	uint32_t tableid;
	int64_t  hw;
} g_rowid_hw[XS_ROWID_HW_MAX];
static int g_rowid_hw_n;
static pthread_mutex_t g_rowid_hw_mu = PTHREAD_MUTEX_INITIALIZER;

/* Leaf lock (never held across a park), a proc critical section so
 * involuntary preemption never parks a holder. */
static void
xs_rowid_hw_lock(void)
{
	xtc_proc_critical_enter();
	(void)pthread_mutex_lock(&g_rowid_hw_mu);
}

static void
xs_rowid_hw_unlock(void)
{
	(void)pthread_mutex_unlock(&g_rowid_hw_mu);
	xtc_proc_critical_leave();
}

int64_t
xstore_next_rowid(struct xsql *db, uint32_t tableid, int64_t floor)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	int64_t r = floor + 1;
	int i;
	if (cx == NULL) return r;
	xs_rowid_hw_lock();
	for (i = 0; i < g_rowid_hw_n; i++)
		if (g_rowid_hw[i].bt == cx->bt && g_rowid_hw[i].tableid == tableid)
			break;
	if (i == g_rowid_hw_n && i < XS_ROWID_HW_MAX) {
		g_rowid_hw[i].bt = cx->bt;
		g_rowid_hw[i].tableid = tableid;
		g_rowid_hw[i].hw = floor;
		g_rowid_hw_n++;
	}
	if (i < g_rowid_hw_n) {
		if (g_rowid_hw[i].hw >= r)
			r = g_rowid_hw[i].hw + 1;
		g_rowid_hw[i].hw = r;
	}
	xs_rowid_hw_unlock();
	return r;
}

static void
xs_rowid_hw_forget(bt_t *bt)
{
	int i;
	xs_rowid_hw_lock();
	for (i = 0; i < g_rowid_hw_n; ) {
		if (g_rowid_hw[i].bt == bt) {
			g_rowid_hw_n--;
			if (i != g_rowid_hw_n) g_rowid_hw[i] = g_rowid_hw[g_rowid_hw_n];
		} else {
			i++;
		}
	}
	xs_rowid_hw_unlock();
}

/* Start of a statement (a SELECT, UPDATE or DELETE): record the one
 * snapshot its reads use -- the base for a write's write-write conflict
 * check (READ COMMITTED and autocommit; snapshot levels use txn_snap) --
 * and, in autocommit, pin it until xstore_stmt_end. */
/* Pin the snapshot an autocommit statement reads at (a transaction's pin,
 * txn_snap <= now, or an as_of pin already covers it); returns it. */
static uint64_t
xs_stmt_pin(xstore_ctx_t *cx, struct xsql *db)
{
	xs_enter_ctx(cx, db);
	cx->stmt_puts = 0;
	cx->last_conflict = 0;       /* an INSERT must not see the last one's */
	xs_rd_err_clear(cx);
	if (!cx->in_txn &&
	    xtc_atomic_load_u64(&cx->read_snap, xtc_mo_relaxed) == 0)
		return cx->stmt_snap = snap_pin_visible(&cx->snap_slot);
	return cx->stmt_snap = xs_visible();
}

/* Pin only (no statement snapshot or conflict base), for a statement
 * that does not use one -- an INSERT, whose INSERT ... SELECT source
 * still reads -- until xstore_stmt_end. */
void
xstore_stmt_pin(struct xsql *db)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	if (cx != NULL) (void)xs_stmt_pin(cx, db);
}

void
xstore_stmt_begin(struct xsql *db)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	if (cx == NULL) return;
	cx->stmt_base = xs_stmt_pin(cx, db);
	cx->last_conflict = 0;
	cx->stmt_puts = 0;
	cx->stmt_first = cx->in_txn && !cx->touched && cx->wn == 0;
}

void
xstore_stmt_end(struct xsql *db)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	if (cx == NULL) return;
	cx->stmt_base = 0;
	if (!cx->in_txn) {
		xs_pin_recompute(cx);    /* drop the statement's pin */
		/* A write's own commit queued index entries its pin kept
		 * from the reap at that commit: reap them now, as before. */
		if (cx->stmt_puts > 0 && XS_IDX_DEAD_HINT() > 0)
			xs_idx_reap(cx->bt);
	}
}

int
xstore_last_conflict(struct xsql *db)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	return cx != NULL && cx->last_conflict;
}

/* The write statement that just failed on a conflict: may it re-run on
 * a fresh snapshot?  Only for a STALE conflict (a deadlock victim would
 * deadlock again), and only where that is invisible to the client:
 *   - a READ COMMITTED transaction (each statement has its own snapshot;
 *     PostgreSQL re-evaluates the row, Oracle restarts the statement;
 *     the caller undid the statement's buffered writes);
 *   - the FIRST access of a snapshot transaction: it has read and written
 *     nothing, so taking its snapshot now is the same as a later BEGIN
 *     (PostgreSQL takes a REPEATABLE READ snapshot at the first
 *     statement too).  Not SERIALIZABLE: its SSI slot holds the old one;
 *   - autocommit before the statement applied any row (applied rows
 *     cannot be undone), which covers every multi-row statement, since
 *     its implicit transaction applies nothing until it commits (the
 *     caller rolled that back).  The re-run first waits until every
 *     commit so far is visible (below).
 * Returns 1 and prepares the re-run, or 0. */
int
xstore_stmt_restart(struct xsql *db)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	if (cx == NULL || cx->last_conflict != XS_CONFL_STALE) return 0;
	if (!cx->in_txn) {
		if (cx->stmt_puts != 0) return 0;
		/* The conflicting commit may be applied but not yet visible
		 * (an older commit is still in flight).  A re-run before it is
		 * would miss it again: an INSERT, which takes no row intent to
		 * wait on, would re-run until the cap.  Nothing is held here,
		 * and in-flight commits never wait on us.  NB the wait is as
		 * bounded as the commits it follows: a commit parked in
		 * wal_commit with no log writer left never becomes visible, so
		 * this wait does not return either, just as that commit's own
		 * xs_wait_visible does not. */
		xs_wait_visible(xtc_atomic_load_u64(&g_xclock, xtc_mo_seq_cst));
		return 1;
	}
	if (cx->txn_iso < XS_ISO_REPEATABLE_READ) return 1;
	if (!cx->stmt_first || cx->serializable || cx->wn != 0) return 0;
	cx->txn_snap = snap_pin_visible(&cx->snap_slot);
	cx->touched = 0;
	cx->rn = 0;
	cx->did_scan = 0;
	return 1;
}

static int xs_commit_ctx(xstore_ctx_t *cx);

/*
 * Statement atomicity in autocommit.  A statement that writes more than
 * one row runs as an implicit transaction: xstore_stmt_implicit_begin
 * opens it before the first write, so each row is buffered and checked
 * as a transaction's is (row intents, UNIQUE against committed rows and
 * the statement's own), and xstore_stmt_implicit_end commits them at one
 * timestamp or discards them all.  A one-row statement is atomic as it
 * is and keeps the direct autocommit write.
 *
 * Its snapshot is the statement's own (stmt_snap, pinned before the
 * statement read anything), at SNAPSHOT isolation, so every buffered
 * write is checked against it: a row committed by another writer after
 * the statement read it fails the commit (SX_BUSY, a conflict the
 * engine re-runs the statement for), and so does a key another INSERT
 * committed after this one's key check: that statement's re-run finds
 * the key and reports UNIQUE, as PostgreSQL does.
 *
 * Its row intents are held to the end of the statement, so two such
 * statements that take the same rows in opposite orders (one in key
 * order, one through an index whose order runs the other way) can
 * deadlock.  The wait-for
 * walk in xs_intent_take finds the cycle at once (xs_ilk_victim) and
 * fails its youngest member with XS_CONFL_HARD, well before the 10 s
 * wait cap.  That statement is not re-run (it could deadlock again);
 * its rows are discarded and the client gets the serialization error,
 * as PostgreSQL reports a deadlock.  In a 2-thread probe of 200 such
 * pairs, one of each pair failed this way, in about 3 ms a pair.
 *
 * No SSI: an autocommit statement is not serializable today, and stays
 * so.  The commit is the
 * transaction commit (xs_commit_ctx), so the log holds the same record a
 * BEGIN ... COMMIT of those rows writes.
 */
int
xstore_stmt_implicit_begin(struct xsql *db)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	if (cx == NULL) return -1;
	xs_enter_ctx(cx, db);
	if (cx->in_txn) return 0;               /* a transaction is open */
	if (XTC_DST_BUG(XTC_DST_BUG_STMTATOMIC))
		return 0;     /* planted 87: each row commits on its own */
	cx->native_autocommit = 0;              /* xs_enter_ctx keeps it open */
	cx->in_txn = 1;
	cx->implicit = 1;
	cx->txn_snap = cx->stmt_snap != 0 ? cx->stmt_snap : xs_visible();
	snap_set(&cx->snap_slot, cx->txn_snap);
	wbuf_clear(cx);
	cx->steal_txn = 0;
	cx->rn = 0;
	cx->did_scan = 0;
	cx->txn_iso = XS_ISO_SNAPSHOT;
	cx->touched = 0;
	cx->serializable = 0;
	cx->ssi = NULL;
	/* Wait for a row's holder before checking the row, as a re-runnable
	 * statement does (see xs_intent_take). */
	cx->stmt_first = 1;
	return 1;
}

/* End the implicit transaction, if one is open: commit its rows when
 * `ok` (the statement succeeded), no read of the statement failed and no
 * kill is pending for the connection's proc (a dead connection must not
 * commit a statement it never answered), else discard them.  Returns 0
 * (committed, or nothing to do), 1 for a UNIQUE violation at the commit
 * (xstore_uniq_err says which), or -1 (discarded, or the commit failed:
 * last_conflict, the read error or the log says why). */
int
xstore_stmt_implicit_end(struct xsql *db, int ok)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	int rc;
	if (cx == NULL || !cx->implicit) return 0;
	cx->native_autocommit = 1;
	if (!ok || xs_stmt_doomed(cx)) {
		cx->implicit = 0;
		(void)xs_rollback_ctx(cx);
		return -1;
	}
	cx->uniq_err[0] = '\0';
	rc = xs_commit_ctx(cx);   /* cx->implicit tells its test hook */
	cx->implicit = 0;
	if (rc == SX_OK) return 0;
	if (rc == SX_BUSY) cx->last_conflict = XS_CONFL_STALE;
	return rc == SX_CONSTRAINT ? 1 : -1;
}

/* True if the connection currently has an open (non-autocommit)
 * transaction -- so the native write path can read the current row's
 * pre-image from the wbuf (read-your-writes) rather than the B-tree. */
int
xstore_in_txn(struct xsql *db)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	if (cx == NULL) return 0;
	xs_enter_ctx(cx, db);
	return cx->in_txn;
}

/*
 * Does the current transaction hold any buffered (uncommitted) write
 * for `tableid`?  When 0, a committed-B-tree scan of that table sees
 * exactly what a vtab cursor would (the wbuf adds nothing), so a native
 * read path may safely run in-transaction; when 1, the wbuf could
 * supersede committed rows and the caller must defer to the merge-aware
 * path.  Returns 0 outside a transaction.
 */
int
xstore_txn_table_dirty(struct xsql *db, uint32_t tableid)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	int i;
	if (cx == NULL) return 0;
	xs_enter_ctx(cx, db);
	if (!cx->in_txn) return 0;
	for (i = 0; i < cx->wn; i++)
		if (cx->wbuf[i].tableid == tableid)
			return 1;
	return 0;
}

#if defined(SQLXTC_VTAB)
static int
xs_update(xsql_vtab *pv, int argc, xsql_value **argv,
    xsql_int64 *pRowid)
{
	xstore_vtab_t *v = (xstore_vtab_t *)pv;
	xstore_ctx_t *cx = v->ctx;

	xs_enter(v);
	/*
	 * Buffer the write; xCommit flushes the whole transaction at ONE
	 * commit timestamp (atomic multi-row commit).  SQLite wraps every
	 * write -- explicit BEGIN..COMMIT or an implicit single-statement
	 * transaction -- in xBegin/xCommit, so a buffer is always open
	 * here.  (Fallback: if no transaction is open, apply immediately.)
	 */
	if (argc == 1) {
		int64_t rid = xsql_value_int64(argv[0]);   /* DELETE */
		if (cx->in_txn)
			return xs_buf_write(cx, v->tableid, rid, NULL, 0, 1);
		return xs_put_pruned(cx, v->tableid, rid, NULL, 0, 1, 0);
	}
	{
		/*
		 * The first declared column is the INTEGER PRIMARY KEY (the
		 * rowid).  For a virtual table SQLite delivers that column's NEW
		 * value in the column slot argv[2] -- NOT in the rowid slot
		 * argv[1], which it leaves at the OLD rowid on an UPDATE.  So the
		 * effective new rowid is argv[2] (its explicit value on INSERT,
		 * or the reassigned pk on UPDATE), and argv[0] is the old rowid
		 * to tombstone when the pk changed.  Using argv[1] here would
		 * silently ignore an UPDATE ... SET <pk> = <new>.
		 *
		 * When argv[2] is NULL (an INSERT with no explicit pk, or pk
		 * NULL), assign the next rowid: one past the larger of the
		 * committed max and any rowid already buffered in this txn, so
		 * successive auto inserts do not collide.
		 *
		 * The payload columns are argv[3..], encoded as the version
		 * record (the pk/rowid is the key, not part of the payload).
		 */
		int64_t rowid;
		uint8_t rec[XS_VMAX];
		int reclen;
		if (xsql_value_type(argv[2]) == SQLITE_NULL) {
			int64_t mx, i;
			if (xstore_max_rowid(cx->bt, v->tableid, &mx) != XTC_OK)
				return SQLITE_IOERR;
			for (i = 0; i < cx->wn; i++)
				if (cx->wbuf[i].tableid == v->tableid &&
				    cx->wbuf[i].rowid > mx)
					mx = cx->wbuf[i].rowid;
			rowid = mx + 1;
		} else {
			rowid = xsql_value_int64(argv[2]);
		}
		reclen = xs_rec_encode(&argv[3], argc - 3, rec, sizeof rec);
		if (reclen < 0)
			return SQLITE_TOOBIG;

		/* An UPDATE that moves the rowid tombstones the old one. */
		if (xsql_value_type(argv[0]) != SQLITE_NULL) {
			int64_t oldid = xsql_value_int64(argv[0]);
			if (oldid != rowid) {
				if (cx->in_txn) (void)xs_buf_write(cx, v->tableid, oldid, NULL, 0, 1);
				else (void)xs_put_pruned(cx, v->tableid, oldid, NULL, 0, 1, 0);
			}
		}
		if (pRowid != NULL) *pRowid = rowid;
		if (cx->in_txn)
			return xs_buf_write(cx, v->tableid, rowid, rec, reclen, 0);
		return xs_put_pruned(cx, v->tableid, rowid, rec, reclen, 0, 0);
	}
}
#endif /* SQLXTC_VTAB */

/*
 * Transaction hooks.  xBegin captures the read snapshot and opens the
 * write buffer; xCommit assigns ONE commit timestamp and flushes every
 * buffered write at it (so a multi-row transaction is atomic and all
 * its rows share an xmin); xRollback discards the buffer.  This is
 * mvcc.c's stage-then-commit, applied to the B-tree-backed store.
 */
#if defined(SQLXTC_VTAB)
static int
xs_begin(xsql_vtab *pv)
{
	xs_enter((xstore_vtab_t *)pv);
	return SX_OK;
}
#endif /* SQLXTC_VTAB */
/*
 * Serializable validation (Cahill SSI pivot detection).  Returns
 * SX_BUSY if this transaction is a dangerous pivot (BOTH an
 * outgoing and an incoming rw-antidependency), else SX_OK, or the
 * XTC_E_* code of a read that failed (rd_err says which table).  Shared
 * by the vtab xSync (2PC phase 1) and the native commit path.
 */
static int
xs_ssi_validate_locked(xstore_ctx_t *cx)
{
	const struct xs_inflight *e;
	int i, r, out_edge = 0, in_edge;
	xs_rkey_t *wr;
	int nwr;
	if (!cx->in_txn || !cx->serializable)
		return SX_OK;
	/*
	 * Outgoing edge: a key we read was overwritten by a transaction
	 * that committed after our snapshot -- read straight from the
	 * shared B-tree's version timestamps, so it sees every committer --
	 * or is being written by a commit still in flight (it holds a
	 * timestamp newer than our snapshot but its versions may not be in
	 * the tree yet).  The caller holds the commit lock, so the in-flight
	 * set is exactly the commits that passed validation before us:
	 * validation and commit are serialized (two transactions in a
	 * write skew can no longer both validate before either is visible).
	 */
	for (i = 0; i < cx->rn && !out_edge; i++) {
		r = xs_newer_than(cx->bt, cx->rset[i].tableid, cx->rset[i].rowid,
		    cx->txn_snap);
		if (r < 0) {                 /* unread: nothing is proven */
			xs_rd_err_tid(cx, cx->rset[i].tableid, r);
			return r;
		}
		if (r > 0) { out_edge = 1; break; }
		for (e = g_inflight; e != NULL; e = e->next)
			if (xs_inflight_has(e, cx->rset[i].tableid, cx->rset[i].rowid)) {
				out_edge = 1;
				break;
			}
	}
	if (cx->did_scan &&
	    xtc_atomic_load_u64(&g_xclock, xtc_mo_relaxed) > cx->txn_snap)
		out_edge = 1;
	if (!out_edge)
		return SX_OK;          /* no outgoing edge -> cannot be a pivot */
	/*
	 * Incoming edge: another concurrent serializable transaction read
	 * a rowid we are about to write.  Reads leave no version in the
	 * B-tree, so this is answered from the in-memory SSI registry,
	 * scanned against our buffered write set.
	 */
	wr = malloc((size_t)(cx->wn ? cx->wn : 1) * sizeof *wr);
	nwr = 0;
	if (wr != NULL)
		for (i = 0; i < cx->wn; i++) {
			wr[nwr].tableid = cx->wbuf[i].tableid;
			wr[nwr].rowid = cx->wbuf[i].rowid;
			nwr++;
		}
	in_edge = ssi_in_conflict(cx->ssi, wr, nwr);
	free(wr);
	return in_edge ? SX_BUSY : SX_OK;   /* pivot -> serialization failure */
}

#if defined(SQLXTC_VTAB)
static int
xs_sync(xsql_vtab *pv)
{
	/*
	 * Serializable validation, in xSync (2PC phase 1) so a failure
	 * rolls the transaction back -- xCommit (phase 2) is too late.
	 *
	 */
	int rc;
	xs_commit_lock();
	rc = xs_ssi_validate_locked(((xstore_vtab_t *)pv)->ctx);
	xs_commit_unlock();
	return rc;
}
#endif /* SQLXTC_VTAB */

/*
 * Serialize the transaction's buffered versions into one WAL record and
 * make it DURABLE before any of them touch the B-tree.  The record is a
 * run of XL_UPDATE sub-records (one per buffered version) followed by an
 * XL_COMMIT, all sharing this transaction's commit timestamp as txn_id;
 * see xlog.h.  Packing the whole transaction into one WAL frame keeps it
 * atomically durable with a single fsync (group commit), exactly the
 * NO-STEAL discipline: a crash either has the complete frame (redo it)
 * or not (nothing reached the B-tree, nothing to undo).
 *
 * Logging before applying is what lets recovery redo the commit from the
 * log (idempotent -- version keys (rowid, ~commit_ts) are immutable).
 * On a loop the commit joins the group-commit batch; off a loop it
 * appends synchronously.  Returns the assigned commit LSN (0 on OOM).
 */
static int
xs_wal_log(xstore_ctx_t *cx, uint64_t ts, uint64_t *clsn)
{
	size_t sz = XL_HDR_LEN;           /* trailing XL_COMMIT */
	uint8_t *rec, *p;
	xl_hdr_t h;
	xl_body_t b;
	int i, n, rc;

	for (i = 0; i < cx->wn; i++) {
		uint16_t vl = cx->wbuf[i].deleted ? 0 : (uint16_t)cx->wbuf[i].len;
		sz += (size_t)XL_HDR_LEN + xl_update_size(vl, 0);
	}
	*clsn = 0;
	rec = malloc(sz);
	if (rec == NULL)
		return XTC_E_NOMEM;           /* not logged: the commit fails */
	p = rec;
	for (i = 0; i < cx->wn; i++) {
		xs_wrec_t *w = &cx->wbuf[i];
		uint16_t vl = w->deleted ? 0 : (uint16_t)w->len;
		h.type = XL_UPDATE; h.txn_id = ts; h.prev_lsn = 0;
		memset(&b, 0, sizeof b);
		b.tableid = w->tableid; b.rowid = w->rowid; b.commit_ts = ts;
		b.flags = w->deleted ? XS_F_DELETED : 0;
		b.redo = vl ? w->data : NULL; b.redo_len = vl;
		if ((n = xl_enc_update(p, sz - (size_t)(p - rec), &h, &b)) < 0) {
			free(rec); return XTC_E_INVAL;
		}
		p += n;
	}
	h.type = XL_COMMIT; h.txn_id = ts; h.prev_lsn = 0;
	if ((n = xl_enc_simple(p, sz - (size_t)(p - rec), &h)) < 0) {
		free(rec); return XTC_E_INVAL;
	}
	p += n;
	rc = xs_wal_emit(rec, (size_t)(p - rec), clsn);
	free(rec);
	return rc;
}

/* STEAL: bring the transaction's spilled payloads back before COMMIT
 * logs them.  One that cannot be read back would go to the log as a
 * record with no payload, so the transaction cannot commit.  SX_OK, or
 * SX_ERROR with the statement's read error set (the caller rolls
 * back). */
static int
xs_commit_unspill(xstore_ctx_t *cx)
{
	int i, rc;
	for (i = 0; i < cx->wn; i++) {
		xs_wrec_t *w = &cx->wbuf[i];
		rc = w->spilled && xs_row_fault() ? SX_ERROR
		    : xs_wrec_ensure(cx, w);
		if (rc != SX_OK) {
			if (rc != SX_NOMEM)
				(void)xs_rd_count(XTC_E_IO);
			xs_rd_err_tid(cx, w->tableid,
			    rc == SX_NOMEM ? XTC_E_NOMEM : XTC_E_IO);
			return SX_ERROR;
		}
	}
	return SX_OK;
}

static int
xs_commit_ctx(xstore_ctx_t *cx)
{
	struct xs_inflight fl;
	struct xs_idx_rt uq[16];
	uint64_t ts, v0;
	uint32_t ubad = 0;
	int i, rc = SX_OK, nuq = 0;
	if (!cx->in_txn)
		return SX_OK;
	if (cx->wn == 0) {
		/* Read-only: SSI can only fail a transaction that writes (an
		 * empty write set has no incoming edge), so no commit lock.
		 * Keep the read set visible to concurrent peers until GC. */
		ssi_commit(cx->ssi, xs_visible());
		cx->ssi = NULL;
		wbuf_clear(cx);
		cx->in_txn = 0;
		xs_pin_recompute(cx);
		return SX_OK;
	}
	{
		if (cx->steal_txn != 0 && xs_commit_unspill(cx) != SX_OK) {
			(void)xs_rollback_ctx(cx);
			return SX_ERROR;
		}
		/* First-committer-wins write-write check, the timestamp, and
		 * the in-flight registration are one atomic step under the
		 * commit lock; the log write and the apply run outside it so
		 * group commit still batches concurrent committers. */
		memset(&fl, 0, sizeof fl);
		fl.ws = cx->wbuf;
		fl.wn = cx->wn;
		fl.cx = cx;
		nuq = xs_uniq_collect(cx->bt, cx, 0, uq, 16);
		/* Serializable validation (SSI pivot), the first-committer-
		 * wins check, UNIQUE re-validation, the timestamp and the
		 * in-flight registration are one atomic step under the commit
		 * lock; a dangerous pivot or a write-write conflict rolls back
		 * with SX_BUSY, a UNIQUE violation with SX_CONSTRAINT. */
		v0 = xs_visible();            /* before the probe: see g_row_cts */
		if ((rc = xs_ww_probe(cx->bt, cx->wbuf, cx->wn, &ubad)) != 0) {
			if (rc < 0) xs_rd_err_tid(cx, ubad, rc);
			(void)xs_rollback_ctx(cx);
			return rc > 0 ? SX_BUSY : SX_ERROR;   /* committed past us */
		}
		xs_commit_lock();
		xs_gate_wait_locked();
		rc = xs_ssi_validate_locked(cx);
		if (rc == SX_OK && (rc = xs_ww_recheck_locked(cx->bt, cx->wbuf,
		    cx->wn, v0, &ubad)) != 0) {
			if (rc < 0) xs_rd_err_tid(cx, ubad, rc);
			else rc = SX_BUSY;
		}
		if (rc != SX_OK) {
			xs_commit_unlock();
			(void)xs_rollback_ctx(cx);
			return rc > 0 ? SX_BUSY : SX_ERROR;   /* rc < 0: unread */
		}
		rc = 0;
		if (nuq > 0)
			rc = xs_uniq_validate_locked(cx->bt, cx, 0, 0, NULL, 0,
			    uq, nuq, &ubad);
		if (rc != 0) {
			struct xs_idx_ent e;
			xs_commit_unlock();
			if (rc < 0)
				xs_idx_err_msg(cx->bt, ubad, rc, cx->uniq_err,
				    sizeof cx->uniq_err);
			else if (xs_idx_get(cx->bt, ubad, &e))
				xs_uniq_msg(&e, cx->uniq_err, sizeof cx->uniq_err);
			(void)xs_rollback_ctx(cx);
			return rc < 0 ? SX_ERROR : SX_CONSTRAINT;
		}
		/* Masked from here to the end: the in-flight entry is on this
		 * stack, so a kill before xs_inflight_remove would leave it
		 * linked (a dangling pointer that pins the read horizon: every
		 * later COMMIT waits forever).  A kill is delivered once the
		 * commit is applied and visible.  Not earlier: a kill must still
		 * reach a committer parked at the checkpoint gate. */
		(void)xtc_mask_enter();
		xs_inflight_add_locked(&fl);  /* one timestamp for the txn */
		xs_uniq_stamp_locked(cx, 0, NULL, 0, 0, uq, nuq, fl.ts);
		xs_commit_unlock();
		ts = fl.ts;
		if (cx->implicit && g_icommit_hook != NULL)
			g_icommit_hook(XS_ICOMMIT_PRELOG);
		if (g_xwal != NULL) {
			uint64_t clsn = 0;
			/* Durable BEFORE apply.  If the log write fails the
			 * transaction is not durable: apply nothing, abort it, and
			 * report the error (the client must not be told it
			 * committed). */
			if (xs_wal_log(cx, ts, &clsn) != XTC_OK) {
				xs_inflight_remove(&fl);
				xs_inflight_unlinked(&fl);   /* fl is on this stack */
				(void)xs_rollback_ctx(cx);
				(void)xtc_mask_leave();
				return SX_ERROR;
			}
			/* Stamp the commit LSN onto every leaf these inserts dirty,
			 * so a physiological leaf after-image (xs_leaf_page) carries a
			 * monotonic page LSN recovery can gate the apply by.  Without
			 * this the leaf image would inherit a stale cur_lsn from the
			 * last autocommit and the page-LSN gate could refuse a newer
			 * image or accept a stale one. */
			if (clsn != 0)
				bt_set_lsn(cx->bt, clsn);
		}
		if (cx->implicit && g_icommit_hook != NULL)
			g_icommit_hook(XS_ICOMMIT_LOGGED);
		for (i = 0; i < cx->wn; i++) {
			uint8_t key[XS_VKLEN];
			uint8_t buf[1 + XS_VMAX];
			uint8_t pre[XS_VMAX]; int prelen;
			xs_wrec_t *w = &cx->wbuf[i];
			/* Capture the committed pre-image BEFORE inserting the new
			 * version (which would otherwise become the newest and hide
			 * it), so index maintenance can remove the stale entry. */
			if (!wbuf_is_newest(cx, i))
				continue;   /* a later write of the row in this txn wins */
			/* An unread pre-image taints the indexes. */
			prelen = xs_idx_preimage(cx->bt, /* RDERR_OK: taints */
			    w->tableid, w->rowid, pre, (int)sizeof pre);
			enc_vkey(w->tableid, w->rowid, ts, key);
			buf[0] = w->deleted ? XS_F_DELETED : 0;
			if (!w->deleted && w->len) memcpy(buf + 1, w->data, w->len);
			if (bt_insert(cx->bt, key, XS_VKLEN, buf,
			    (uint16_t)(1 + (w->deleted ? 0 : w->len))) != XTC_OK)
				rc = SX_ERROR;
			else {
				xs_idx_maintain_ts(cx->bt, w->tableid, w->rowid,
				    w->data, w->deleted ? 0 : (int)w->len, w->deleted,
				    prelen > 0 ? pre : NULL, prelen, ts);
				if (cx->autovacuum)
					xs_maybe_prune(cx->bt, w->tableid, w->rowid);
			}
		}
		ssi_commit(cx->ssi, ts);     /* publish commit_ts for SSI peers */
		xs_inflight_remove(&fl);
		xs_idx_reap(cx->bt);
		/* COMMIT returns once it is visible -- which also means fl (on
		 * this stack) is unlinked: the horizon never passes it. */
		xs_wait_visible(ts);
	}
	cx->ssi = NULL;
	wbuf_clear(cx);
	cx->in_txn = 0;
	xs_pin_recompute(cx);     /* txn done: drop (or re-pin as_of) */
	(void)xtc_mask_leave();   /* may not return: a latched kill */
	return rc;
}
#if defined(SQLXTC_VTAB)
static int
xs_commit(xsql_vtab *pv)
{
	return xs_commit_ctx(((xstore_vtab_t *)pv)->ctx);
}
#endif /* SQLXTC_VTAB */
static int
xs_rollback_ctx(xstore_ctx_t *cx)
{
	ssi_abort(cx->ssi);
	cx->ssi = NULL;
	wbuf_clear(cx);
	cx->in_txn = 0;
	xs_pin_recompute(cx);
	return SX_OK;
}
#if defined(SQLXTC_VTAB)
static int
xs_rollback(xsql_vtab *pv)
{
	return xs_rollback_ctx(((xstore_vtab_t *)pv)->ctx);
}
#endif /* SQLXTC_VTAB */

/*
 * Native transaction control -- what the live path calls when it sees a
 * COMMIT / ROLLBACK statement that may carry native (VDBE-free) writes
 * buffered in this connection's wbuf.  The live path runs these BEFORE
 * the SQLite COMMIT/ROLLBACK statement, so the native writes flush /
 * discard while in_txn is still set; the subsequent SQLite statement
 * just flips autocommit back on (its vtab xCommit/xRollback then no-ops
 * on the now-empty, closed buffer).  No-ops when not in a transaction or
 * not xstore-backed.
 */
int
xstore_commit(struct xsql *db)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	if (cx == NULL) return 0;
	if (cx->native_mode) cx->native_autocommit = 1;   /* end explicit txn */
	if (!cx->in_txn) return 0;
	cx->uniq_err[0] = '\0'; /* a failed commit's reason, if any */
	{
		int rc = xs_commit_ctx(cx);
		if (rc == SX_OK) return 0;
		if (rc == SX_BUSY) return SX_BUSY;   /* serialization conflict */
		if (rc == SX_CONSTRAINT) return SX_CONSTRAINT;   /* uniq_err set */
		return -1;
	}
}
int
xstore_rollback(struct xsql *db)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	if (cx == NULL) return 0;
	if (cx->native_mode) cx->native_autocommit = 1;   /* end explicit txn */
	if (!cx->in_txn) return 0;
	return xs_rollback_ctx(cx) == SX_OK ? 0 : -1;
}

/*
 * Native-driver transaction ownership.  A native sx_stmt driver (which
 * never calls the SQLite VDBE) calls xstore_native_mode(db, 1) once to
 * take ownership of autocommit from SQLite, then drives BEGIN / COMMIT
 * / ROLLBACK directly:
 *   - xstore_native_begin clears native_autocommit (opening an explicit
 *     txn) and enters the ctx so the snapshot is captured now;
 *   - xstore_commit / xstore_rollback (above) set native_autocommit
 *     back to 1 and flush / discard the txn.
 * In native_mode xs_enter_ctx consults native_autocommit, so the
 * autocommit DML path (a single statement outside an explicit txn) and
 * the savepoint API work without any SQLite autocommit flag.  No-op /
 * returns -1 when not xstore-backed.
 */
int
xstore_native_mode(struct xsql *db, int on)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	if (cx == NULL) return -1;
	cx->native_mode = on ? 1 : 0;
	if (on) cx->native_autocommit = cx->in_txn ? 0 : 1;
	return 0;
}

/* Set the connection isolation level from a name ("read uncommitted",
 * "read committed", "repeatable read", "snapshot", "serializable").  The
 * native C-API equivalent of the xstore_isolation() SQL function.
 * Returns 0 on success, -1 if the connection or name is unknown. */
static int
xs_iso_parse(const char *level)
{
	if (level == NULL) return -1;
	if (ciprefix(level, "read un"))      return XS_ISO_READ_UNCOMMITTED;
	if (ciprefix(level, "read c"))       return XS_ISO_READ_COMMITTED;
	if (ciprefix(level, "rep"))          return XS_ISO_REPEATABLE_READ;
	if (ciprefix(level, "snap"))         return XS_ISO_SNAPSHOT;
	if (ciprefix(level, "ser"))          return XS_ISO_SERIALIZABLE;
	return -1;
}

int
xstore_set_isolation(struct xsql *db, const char *level)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	int lvl = xs_iso_parse(level);
	if (cx == NULL || lvl < 0) return -1;
	cx->isolation = lvl;            /* takes effect at the next txn */
	return 0;
}

/* SET TRANSACTION ISOLATION LEVEL: the level of the CURRENT transaction
 * only (the session level resumes at the next one).  Must be issued
 * inside a transaction before it reads or writes anything.  Returns 0,
 * -1 for an unknown connection/level, -2 outside a transaction, -3 if
 * the transaction already ran a query. */
int
xstore_set_txn_isolation(struct xsql *db, const char *level)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	int lvl = xs_iso_parse(level);
	if (cx == NULL || lvl < 0) return -1;
	xs_enter_ctx(cx, db);
	if (!cx->in_txn) return -2;
	if (cx->touched || cx->wn > 0) return -3;
	cx->txn_iso = lvl;
	cx->serializable = (lvl == XS_ISO_SERIALIZABLE);
	if (cx->serializable && cx->ssi == NULL)
		cx->ssi = ssi_begin(cx->txn_snap);
	else if (!cx->serializable && cx->ssi != NULL) {
		ssi_abort(cx->ssi);
		cx->ssi = NULL;
	}
	return 0;
}

/* Native C-API equivalents of the MVCC SQL functions (the functions
 * stay for the vtab; these let a SQLite-free connection drive the same
 * machinery). */

/* Current HLC clock (xstore_now()). */
int64_t
xstore_clock_now(void)
{
	return (int64_t)xs_visible();   /* the newest fully committed stamp */
}

/* Pin the connection's reads at timestamp ts (xstore_as_of(ts)); ts 0
 * releases the pin (back to latest committed). */
int
xstore_as_of(struct xsql *db, int64_t ts)
{
	xstore_ctx_t *c = xstore_ctx_of(db);
	if (c == NULL) return -1;
	if (ts < 0) ts = 0;
	xtc_atomic_store_u64(&c->read_snap, (uint64_t)ts, xtc_mo_relaxed);
	xs_pin_recompute(c);
	return 0;
}

/* Garbage-collect versions below the global snapshot horizon
 * (xstore_gc()).  Returns the number of versions reclaimed, or -1. */
int
xstore_gc_run(struct xsql *db)
{
	xstore_ctx_t *c = xstore_ctx_of(db);
	uint64_t now, horizon;
	int reclaimed = 0;
	if (c == NULL) return -1;
	now = xs_visible();             /* readers may sit at the horizon */
	horizon = snap_horizon(now);
	/* GC stops on a read error (counted): versions leak, no more. */
	(void)xs_gc(c->bt, horizon, &reclaimed);   /* RDERR_OK: a leak */
	if (horizon > xtc_atomic_load_u64(&g_gc_horizon, xtc_mo_relaxed))
		xtc_atomic_store_u64(&g_gc_horizon, horizon, xtc_mo_relaxed);
	return reclaimed;
}

/* Version GC for a clean close (xstore.h). */
uint64_t
xstore_gc_close(bt_t *bt)
{
	uint64_t horizon;
	int reclaimed = 0;
	if (bt == NULL) return 0;
	horizon = snap_horizon(xs_visible());
	/* GC stops on a read error (counted): versions leak, no more. */
	(void)xs_gc(bt, horizon, &reclaimed);   /* RDERR_OK: a leak */
	if (horizon > xtc_atomic_load_u64(&g_gc_horizon, xtc_mo_relaxed))
		xtc_atomic_store_u64(&g_gc_horizon, horizon, xtc_mo_relaxed);
	return (uint64_t)reclaimed;
}

/* The autovacuum setting new connections start with (see
 * xstore_autovacuum_default in xstore.h). */
static int g_av_default;

void
xstore_autovacuum_default(int on)
{
	xtc_atomic_store_i32(&g_av_default, on ? 1 : 0, xtc_mo_relaxed);
}

/* Enable/disable autovacuum for the connection (xstore_autovacuum(on)). */
int
xstore_autovacuum_set(struct xsql *db, int on)
{
	xstore_ctx_t *c = xstore_ctx_of(db);
	if (c == NULL) return -1;
	c->autovacuum = on ? 1 : 0;
	return c->autovacuum;
}

/* STEAL staging rows in the tree (test hook: 0 once no transaction
 * that spilled is open). */
uint64_t
xstore_stage_rows(bt_t *bt)
{
	bt_cursor_t *cur = NULL;
	uint8_t startk[XS_VKLEN];
	uint64_t n = 0;
	if (bt == NULL) return 0;
	enc_vkey(XS_STAGE_TID, INT64_MIN, ~(uint64_t)0, startk);
	/* A test metric (staging keys left): short on a read error. */
	if (bt_cursor_open(bt, startk, XS_VKLEN, &cur) != XTC_OK) /* RDERR_OK: metric */
		return 0;
	for (;;) {
		const void *k = NULL, *v = NULL;
		uint16_t kl = 0, vl = 0;
		if (bt_cursor_next(cur, &k, &kl, &v, &vl) != XTC_OK || /* RDERR_OK: metric */
		    kl != XS_VKLEN ||
		    dec_tableid((const uint8_t *)k) != XS_STAGE_TID)
			break;
		n++;
	}
	bt_cursor_close(cur);
	return n;
}

/* Total autovacuum prunes so far (xstore_prune_count()). */
int64_t
xstore_prune_count(void)
{
	return (int64_t)xtc_atomic_load_u64(&g_av_prunes, xtc_mo_relaxed);
}
int
xstore_native_begin(struct xsql *db)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	if (cx == NULL) return -1;
	cx->native_mode = 1;
	if (cx->in_txn) return 0;        /* already in a txn */
	cx->native_autocommit = 0;       /* explicit txn open */
	xs_enter_ctx(cx, db);            /* capture the snapshot now */
	return 0;
}

/*
 * Nested-transaction (savepoint) hooks.  SQLite assigns each open
 * savepoint a non-negative level and drives:
 *   xSavepoint(n)   open savepoints through level n (record a mark)
 *   xRelease(n)     close savepoints >= n, merging them into the
 *                   parent (their buffered writes simply remain)
 *   xRollbackTo(n)  undo everything done since savepoint n opened
 *                   (truncate wbuf/rset back to its recorded mark)
 * Levels stack, so sp_wn[i]/sp_rn[i] hold the write-buffer and
 * read-set positions captured when level i opened.  Because writes
 * are appended newest-wins and reads appended, truncation alone
 * restores the exact pre-savepoint state -- the Berkeley DB child
 * abort, expressed on the buffered write set.
 */
static int
sp_reserve(xstore_ctx_t *cx, int level)
{
	if (level + 1 > cx->sp_cap) {
		int nc = cx->sp_cap ? cx->sp_cap * 2 : 8;
		int *nw, *nr;
		while (nc < level + 1)
			nc *= 2;
		nw = realloc(cx->sp_wn, (size_t)nc * sizeof *nw);
		if (nw == NULL)
			return SX_NOMEM;
		cx->sp_wn = nw;
		nr = realloc(cx->sp_rn, (size_t)nc * sizeof *nr);
		if (nr == NULL)
			return SX_NOMEM;
		cx->sp_rn = nr;
		cx->sp_cap = nc;
	}
	return SX_OK;
}

/* Savepoint operations on a txn context (shared by the vtab callbacks
 * and the public db-handle wrappers below).  level is 0-based. */
static int
sp_open_ctx(xstore_ctx_t *cx, int level)
{
	int i, rc;
	if (level < 0)
		return SX_OK;
	if ((rc = sp_reserve(cx, level)) != SX_OK)
		return rc;
	/* Record the current marks for the newly opened level(s).  A
	 * statement may open several levels at once; fill the gap so each
	 * captures the same current position. */
	for (i = cx->sp_n; i <= level; i++) {
		cx->sp_wn[i] = cx->wn;
		cx->sp_rn[i] = cx->rn;
	}
	cx->sp_n = level + 1;
	return SX_OK;
}

static int
sp_release_ctx(xstore_ctx_t *cx, int level)
{
	/* Close savepoints >= level.  Their writes merge into the parent
	 * (kept in wbuf), so there is nothing to undo -- just forget the
	 * marks. */
	if (level < 0)
		return SX_OK;
	if (level < cx->sp_n)
		cx->sp_n = level;
	return SX_OK;
}

static int
sp_rollback_to_ctx(xstore_ctx_t *cx, int level)
{
	/* Undo everything since savepoint `level` opened: truncate the
	 * write buffer and read set back to the marks captured then.  The
	 * savepoint itself stays open (it may be rolled back to again), so
	 * keep levels [0, level].  Deeper marks are discarded. */
	if (level < 0 || level >= cx->sp_n)
		return SX_OK;
	wbuf_truncate(cx, cx->sp_wn[level]);
	if (cx->sp_rn[level] < cx->rn)
		cx->rn = cx->sp_rn[level];
	cx->sp_n = level + 1;
	return SX_OK;
}

#if defined(SQLXTC_VTAB)
static int
xs_savepoint(xsql_vtab *pv, int level)
{
	return sp_open_ctx(((xstore_vtab_t *)pv)->ctx, level);
}
#endif /* SQLXTC_VTAB */

#if defined(SQLXTC_VTAB)
static int
xs_release(xsql_vtab *pv, int level)
{
	return sp_release_ctx(((xstore_vtab_t *)pv)->ctx, level);
}
#endif /* SQLXTC_VTAB */

#if defined(SQLXTC_VTAB)
static int
xs_rollback_to(xsql_vtab *pv, int level)
{
	return sp_rollback_to_ctx(((xstore_vtab_t *)pv)->ctx, level);
}
#endif /* SQLXTC_VTAB */

/* Public savepoint API for the native driver (native_mode).  level is
 * 0-based.  In native_mode xs_enter_ctx consults native_autocommit, so
 * a savepoint opened after xstore_native_begin operates on the open
 * txn's wbuf -- identical semantics to the vtab xSavepoint/xRelease/
 * xRollbackTo callbacks, which delegate to the same sp_*_ctx helpers.
 * Return 0 / -1. */
int
xstore_savepoint(struct xsql *db, int level)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	if (cx == NULL) return -1;
	return sp_open_ctx(cx, level) == SX_OK ? 0 : -1;
}
int
xstore_release(struct xsql *db, int level)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	if (cx == NULL) return -1;
	return sp_release_ctx(cx, level) == SX_OK ? 0 : -1;
}
int
xstore_rollback_to(struct xsql *db, int level)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	if (cx == NULL) return -1;
	return sp_rollback_to_ctx(cx, level) == SX_OK ? 0 : -1;
}

/* Largest rowid in `tableid` INCLUDING this connection's uncommitted
 * buffered writes -- the auto-rowid base for an in-txn INSERT, so a
 * buffered row's rowid is not reused (matching the vtab xUpdate auto-
 * rowid path).  Outside a txn this equals xstore_max_rowid.  Sets *out
 * (0 for an empty table / unknown db) and returns 0, or -1 when a read
 * failed (the statement's read error is set; *out is 0 and must not be
 * used). */
static uint64_t g_wmax_scanned; /* wbuf entries scanned (metric) */

uint64_t
xstore_wbuf_scan_count(void)
{
	return xtc_atomic_load_u64(&g_wmax_scanned, xtc_mo_relaxed);
}

int
xstore_max_rowid_txn(struct xsql *db, uint32_t tableid, int64_t *out)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	int64_t mx;
	int i, rc;
	*out = 0;
	if (cx == NULL) return 0;
	rc = xstore_max_rowid(cx->bt, tableid, &mx);
	if (rc != XTC_OK) {
		xs_rd_err_tid(cx, tableid, rc);
		return -1;
	}
	if (!cx->wmax_ok) {                  /* rebuild after a truncate */
		cx->wmax_n = 0;
		cx->wmax_ok = 1;
		for (i = 0; i < cx->wn && cx->wmax_ok; i++)
			if (!wmax_note(cx, cx->wbuf[i].tableid, cx->wbuf[i].rowid))
				cx->wmax_ok = 0;
		xtc_atomic_fetch_add_u64(&g_wmax_scanned, (uint64_t)i,
		    xtc_mo_relaxed);
	}
	if (cx->wmax_ok) {
		for (i = 0; i < cx->wmax_n; i++)
			if (cx->wmax[i].tableid == tableid && cx->wmax[i].max > mx)
				mx = cx->wmax[i].max;
		*out = mx;
		return 0;
	}
	for (i = 0; i < cx->wn; i++)        /* shortcut: > 8 tables in one txn scans */
		if (cx->wbuf[i].tableid == tableid && cx->wbuf[i].rowid > mx)
			mx = cx->wbuf[i].rowid;
	xtc_atomic_fetch_add_u64(&g_wmax_scanned, (uint64_t)cx->wn,
	    xtc_mo_relaxed);
	*out = mx;
	return 0;
}

/* Is `rowid` a live committed row of `tableid` (newest version present
 * and not a tombstone)?  1 if live, 0 if not, or the XTC_E_* code of a
 * read that failed: the row is then unknown, and an INSERT that took it
 * for absent would put in a duplicate primary key.  Used by the
 * primary-key checks of INSERT. */
static int
xs_row_live(bt_t *bt, uint32_t tableid, int64_t rowid)
{
	bt_cursor_t *cur = NULL;
	uint8_t startk[XS_VKLEN];
	const void *k = NULL, *vv = NULL;
	uint16_t klen = 0, vl = 0;
	int live = 0, rc;
	if (bt == NULL) return 0;
	enc_vkey(tableid, rowid, ~(uint64_t)0, startk);   /* newest version first */
	rc = bt_cursor_open(bt, startk, XS_VKLEN, &cur);
	if (rc == XTC_OK) {
		rc = bt_cursor_next(cur, &k, &klen, &vv, &vl);
		if (rc == XTC_OK && xs_row_fault())
			rc = XTC_E_IO;
		if (rc != XTC_OK)
			bt_cursor_close(cur);
	}
	if (rc != XTC_OK && rc != XTC_E_NOTFOUND) {
		(void)xs_rd_count(rc);
		/* planted 83: the unread row is absent, so the key is free */
		return XTC_DST_BUG(XTC_DST_BUG_PKREAD) ? 0 : rc;
	}
	if (rc != XTC_OK)
		return 0;                       /* the end of the tree */
	if (klen == XS_VKLEN &&
	    dec_tableid((const uint8_t *)k) == tableid &&
	    dec_rowid((const uint8_t *)k) == rowid)
		live = !(vl >= 1 && (((const uint8_t *)vv)[0] & XS_F_DELETED));
	bt_cursor_close(cur);
	return live;
}

/* Does `rowid` exist as a LIVE row of `tableid`, accounting for this
 * connection's uncommitted buffered writes?  A buffered tombstone hides
 * a committed row; a buffered upsert reveals one.  Lets the native
 * INSERT enforce PK uniqueness INSIDE a transaction (where a plain
 * committed-B-tree probe would miss buffered rows).  Returns 1 if live,
 * 0 otherwise, or -1 when the row could not be read (rd_err says why). */
int
xstore_row_exists_txn(struct xsql *db, uint32_t tableid, int64_t rowid)
{
	xstore_ctx_t *cx = xstore_ctx_of(db);
	int r;
	if (cx == NULL) return 0;
	if (cx->in_txn) {
		const xs_wrec_t *w = wbuf_find(cx, tableid, rowid);
		if (w != NULL)
			return w->deleted ? 0 : 1;   /* buffered write decides */
	}
	r = xs_row_live(cx->bt, tableid, rowid);
	if (r < 0) {
		xs_rd_err_tid(cx, tableid, r);
		return -1;
	}
	return r;
}

/*
 * Native DDL over the xstore catalog (subsystem D, for the native
 * driver -- no SQLite vtab create).  xstore_create_table allocates a
 * tableid and persists the column schema (coldefs = the comma-joined
 * column-def list, first column the INTEGER PRIMARY KEY), exactly as
 * the vtab xConnect does via xs_cat_find_or_create -- so vexec / the
 * native write path can then operate on the table with no SQLite
 * schema.  Returns the tableid (>0) or 0 on error / already-exists
 * (find_or_create returns the existing id, which is fine: idempotent).
 * xstore_drop_table writes a delete-marked catalog row (the lookup
 * skips it, like a renamed-away entry) and forgets the cache.  Returns
 * 0 / -1.  NOTE: these bypass SQLite's schema entirely; they are for
 * the native driver, where there is no VDBE to keep in sync.
 */
uint32_t
xstore_create_table(struct xsql *db, const char *name, const char *coldefs,
    int *created)
{
	bt_t *bt = xstore_bt_of(db);
	uint32_t id;
	int rderr;
	xs_cat_gen_bump();
	if (created != NULL) *created = 0;
	if (bt == NULL || name == NULL) return 0;
	id = xs_cat_find_or_create(bt, name, coldefs, created, &rderr);
	if (id == 0 && rderr != 0)
		xstore_read_err_catalog(db, rderr);
	return id;
}

/* Register a view (CREATE VIEW name AS <select>): record the SELECT
 * text keyed by (bt, name).  Returns 1 on success, 0 on failure (full
 * registry / too-long SQL / missing bt). */
int
xstore_create_view(struct xsql *db, const char *name, const char *select_sql)
{
	bt_t *bt = xstore_bt_of(db);
	int i, rc = 0;
	xs_cat_gen_bump();
	if (bt == NULL || name == NULL || select_sql == NULL) return 0;
	if (strlen(select_sql) >= sizeof g_view[0].sql) return 0;
	xs_cat_lock();
	for (i = 0; i < g_view_n; i++)          /* replace if redefined */
		if (g_view[i].bt == bt && strcasecmp(g_view[i].name, name) == 0) {
			snprintf(g_view[i].sql, sizeof g_view[i].sql, "%s", select_sql);
			rc = 1; break;
		}
	if (rc == 0 && g_view_n < XS_VIEW_MAX) {
		struct xs_view_ent *v = &g_view[g_view_n++];
		v->bt = bt;
		snprintf(v->name, sizeof v->name, "%s", name);
		snprintf(v->sql, sizeof v->sql, "%s", select_sql);
		rc = 1;
	}
	if (rc)
		xtc_atomic_store_i32(&g_view_any, 1, xtc_mo_release);
	xs_cat_unlock();
	return rc;
}

/* Look up a view's SELECT text for (bt, name).  Copies into out (cap
 * bytes) and returns 1 if found, 0 otherwise. */
int
xstore_view_sql(bt_t *bt, const char *name, char *out, int cap)
{
	int i, rc = 0;
	if (bt == NULL || name == NULL || out == NULL || cap <= 0) return 0;
	/* No view was ever created (the common case): no lock.  The flag is
	 * set (under the cat lock) before CREATE VIEW returns, so a lookup
	 * that starts after it sees the view; a lookup racing the CREATE is
	 * equivalent to running just before it. */
	if (xtc_atomic_load_i32(&g_view_any, xtc_mo_acquire) == 0)
		return 0;
	xs_cat_lock();
	for (i = 0; i < g_view_n; i++)
		if (g_view[i].bt == bt && strcasecmp(g_view[i].name, name) == 0) {
			snprintf(out, (size_t)cap, "%s", g_view[i].sql);
			rc = 1; break;
		}
	xs_cat_unlock();
	return rc;
}

/* Drop a view (DROP VIEW): forget its registry entry.  Idempotent. */
int
xstore_drop_view(struct xsql *db, const char *name)
{
	bt_t *bt = xstore_bt_of(db);
	int i;
	xs_cat_gen_bump();
	if (bt == NULL || name == NULL) return 0;
	xs_cat_lock();
	for (i = 0; i < g_view_n; i++)
		if (g_view[i].bt == bt && strcasecmp(g_view[i].name, name) == 0) {
			g_view_n--;
			if (i != g_view_n) g_view[i] = g_view[g_view_n];
			break;
		}
	xs_cat_unlock();
	return 1;
}

/* ---- secondary-index catalog + API ------------------------------- */

/*
 * The id in a high-water row's payload ("\0<decimal>"), or 0 if the
 * payload is not one.  The catalog scans see the payload with its flag
 * byte in front; xs_read_rec strips it.
 */
static uint32_t
xs_hw_parse(const uint8_t *p, int n)
{
	char num[16];
	if (n < 2 || n - 1 >= (int)sizeof num || p[0] != 0)
		return 0;
	memcpy(num, p + 1, (size_t)(n - 1));
	num[n - 1] = '\0';
	return (uint32_t)strtoul(num, NULL, 10);
}

/*
 * Raise the high-water row of catalog `cat` (XS_CAT_TABLEID or
 * XS_IDXCAT_TABLEID) to at least `id`.  Called by a DROP before it
 * writes its tombstone, so a crash between the two leaves the object in
 * place and the mark already past it.  Holds the cat lock across the
 * read and the write, so two DROPs cannot lower the mark.  Returns
 * SX_OK, or the xs_put error or the read's XTC_E_* code (the DROP must
 * then fail).
 */
static int
xs_hw_raise(bt_t *bt, uint32_t cat, uint32_t id)
{
	uint8_t cur[16], val[16];
	int n, rc = SX_OK;
	xs_cat_lock();
	n = xs_row_fault() ? XTC_E_IO : xs_read_rec(bt, cat, 0, cur,
	    (int)sizeof cur);
	if (n < 0) {                  /* the mark is unknown: do not lower it */
		xs_cat_unlock();
		return xs_rd_count(n);
	}
	if (xs_hw_parse(cur, n) < id) {
		val[0] = 0;
		n = 1 + snprintf((char *)val + 1, sizeof val - 1, "%u",
		    (unsigned)id);
		rc = xs_put(bt, cat, 0, val, n, 0);
	}
	xs_cat_unlock();
	return rc;
}

/*
 * DST crash point between a DROP's high-water mark and its tombstone:
 * under the simulator with buggify on, 1 on the site's seeded coin, and
 * the DROP then stops there as if the tombstone's write had failed (the
 * test crashes the store next).  The object must survive, and its id
 * stay handed out.  A single relaxed load in production.
 */
static int
xs_drop_gap(const char *site)
{
	return XTC_SIM_BUGGIFY(site) && xtc_sim_buggify_fault(1000);
}

/*
 * The largest table id that still has a row version in the tree, or 0.
 * Version keys of the user tables sort below the index namespace
 * (XS_IDX_LEAD), so this is the key just below it: one descent.  It
 * covers a store from before the high-water row, where GC already
 * removed a dropped table's tombstone and left its rows.  XTC_OK with
 * *out, or a read's XTC_E_* code.
 */
static int
xs_data_max_id(bt_t *bt, uint32_t *out)
{
	uint8_t hi[1] = { XS_IDX_LEAD }, k[XS_VKLEN + 8];
	uint16_t klen = 0;
	int rc;
	*out = 0;
	rc = bt_seek_lt(bt, hi, 1, k, (uint16_t)sizeof k, &klen, NULL, 0, NULL);
	if (rc == XTC_OK && xs_row_fault())
		rc = XTC_E_IO;
	if (rc != XTC_OK)
		return rc == XTC_E_NOTFOUND ? XTC_OK : xs_rd_count(rc);
	if (klen == XS_VKLEN)
		*out = dec_tableid(k);
	return XTC_OK;
}

/*
 * The largest id catalog `cat` has handed out: its largest rowid, its
 * high-water row, and (for the table catalog) the largest table id with
 * rows left in the tree.  The next allocation takes one past it.  Covers
 * ids recovered from the WAL, since the catalog is replayed before any
 * connect.  XTC_OK with *out, or a read's XTC_E_* code: an id read as
 * "max 0" would hand out one already in use.
 */
static int
xs_cat_high(bt_t *bt, uint32_t cat, uint32_t *out)
{
	bt_cursor_t *cur = NULL;
	uint32_t maxid = 0, hw = 0, data = 0;
	int seen0 = 0, rc;

	*out = 0;
	if ((rc = xs_cat_open(bt, cat, &cur)) != XTC_OK)
		return rc == XTC_E_NOTFOUND ? XTC_OK : rc;
	for (;;) {
		const void *k = NULL, *vv = NULL;
		uint16_t klen = 0, vl = 0;
		int64_t rid;
		rc = xs_cat_next(cur, &k, &klen, &vv, &vl);
		if (rc != XTC_OK || klen != XS_VKLEN)
			break;
		if (dec_tableid((const uint8_t *)k) != cat)
			break;                       /* past the catalog */
		rid = dec_rowid((const uint8_t *)k);
		if (rid == 0 && !seen0) {
			/* The newest version of the high-water row. */
			seen0 = 1;
			if (vl >= 1 && !(((const uint8_t *)vv)[0] & XS_F_DELETED))
				hw = xs_hw_parse((const uint8_t *)vv + 1, vl - 1);
		}
		if (rid > 0 && (uint32_t)rid > maxid)
			maxid = (uint32_t)rid;
	}
	bt_cursor_close(cur);
	if (rc != XTC_OK && rc != XTC_E_NOTFOUND)
		return rc;
	if (cat == XS_CAT_TABLEID && (rc = xs_data_max_id(bt, &data)) != XTC_OK)
		return rc;
	if (data > hw)
		hw = data;
#if XTC_DST_BUG(XTC_DST_BUG_IDREUSE)
	hw = 0;       /* planted: a dropped id comes back after GC */
#endif
	*out = hw > maxid ? hw : maxid;
	return XTC_OK;
}

/* The largest index id handed out (xs_cat_high over the index catalog),
 * or a larger one allocated in-process whose catalog row is not visible
 * yet.  The next index takes one past it, so an id is never reused.
 * XTC_OK with *out, or a read's XTC_E_* code. */
static int
xs_idxcat_max_id(bt_t *bt, uint32_t *out)
{
	uint32_t maxid;
	int i, rc = xs_cat_high(bt, XS_IDXCAT_TABLEID, &maxid);
	if (rc != XTC_OK)
		return rc;
	xs_cat_lock();
	for (i = 0; i < g_idx_n; i++)
		if (g_idx[i].bt == bt && g_idx[i].rt.idx_id > maxid)
			maxid = g_idx[i].rt.idx_id;
	xs_cat_unlock();
	*out = maxid;
	return XTC_OK;
}

/* Split a comma-joined column list into col[] (up to XS_IDX_NCOL).
 * Returns the count, or -1 on overflow. */
static int
xs_split_cols(const char *csv, char col[][64])
{
	int n = 0, k;
	const char *p = csv;
	while (*p) {
		if (n >= XS_IDX_NCOL) return -1;
		k = 0;
		while (*p && *p != ',') {
			if (k < 63) col[n][k++] = *p;
			p++;
		}
		col[n][k] = '\0';
		if (k > 0) n++;
		if (*p == ',') p++;
	}
	return n;
}

/* Add (or replace) an in-process index-cache entry, resolving its table
 * id and payload map now (a table not resolvable yet keeps tableid 0:
 * never maintained, never chosen).  XTC_OK, or the error of a catalog
 * read that failed: the entry is not added then, since an unresolved
 * index is neither maintained nor enforced. */
static int
xs_idx_cache_add(bt_t *bt, uint32_t idx_id, const char *name, const char *table,
    char col[][64], int ncol, uint32_t descmask, int unique, char kind,
    const uint64_t *sipk)
{
	struct xs_idx_ent e;
	int i, r;
	memset(&e, 0, sizeof e);
	e.bt = bt;
	e.rt.idx_id = idx_id;
	e.rt.ncol = ncol;
	e.rt.descmask = descmask;
	e.rt.unique = unique;
	e.rt.kind = kind;
	e.rt.kmax = (int)bt_max_key(bt);
	e.rt.k0 = sipk[0];
	e.rt.k1 = sipk[1];
	snprintf(e.name, sizeof e.name, "%s", name);
	snprintf(e.table, sizeof e.table, "%s", table);
	for (i = 0; i < ncol; i++)
		snprintf(e.col[i], sizeof e.col[i], "%s", col[i]);
	r = xstore_table_id(bt, table, &e.rt.tableid);
	if (r < 0)
		return r;                        /* unread: not unresolved */
	r = r == 0 ? -1 : xs_idx_resolve(bt, table, e.col, ncol, e.rt.pay);
	if (r < -1)
		return r;
	if (r != 0) {
		/* Not maintained while unresolved: if its table appears later
		 * the index would be trusted with no entries -- do not. */
		e.rt.tableid = 0;
		xtc_atomic_store_i32(&g_idx_taint, 1, xtc_mo_release);
	}
	xs_cat_lock();
	for (i = 0; i < g_idx_n; i++)
		if (g_idx[i].bt == bt && strcmp(g_idx[i].name, name) == 0)
			break;
	if (i < XS_IDX_MAX) g_idx[i] = e;
	if (i == g_idx_n && g_idx_n < XS_IDX_MAX) XS_IDX_N_SET(g_idx_n + 1);
	XS_CATC_BUMP();
	xs_cat_unlock();
	return XTC_OK;
}

/* Parse an index-catalog value: the current six-field form
 * "<name>\0<table>\0<c1,c2..>\0<u>\0<descmask>\0<kind>" or the 1.57
 * four-field form "<name>\0<table>\0<col>\0<u>" (one column, ASC, a
 * CREATE INDEX).  Pointers alias `v`, which must hold a NUL after the
 * last field (the caller copies it into a terminated buffer). */
static int
xs_idxcat_parse(const char *v, int vl, const char **name, const char **table,
    const char **cols, int *unique, uint32_t *descmask, char *kind,
    uint64_t *sipk)
{
	int i, seg = 0;
	const char *f[7];
	char *end = NULL;
	f[0] = v;
	for (i = 0; i < vl && seg < 6; i++)
		if (v[i] == 0) f[++seg] = v + i + 1;
	if (seg < 3) return 0;
	*name = f[0]; *table = f[1]; *cols = f[2];
	*unique = (f[3][0] == '1');
	*descmask = seg >= 4 ? (uint32_t)strtoul(f[4], NULL, 10) : 0;
	*kind = (seg >= 5 && f[5][0] != '\0') ? f[5][0] : 'i';
	sipk[0] = sipk[1] = 0; /* an index from before format 3 */
	if (seg >= 6) {
		sipk[0] = strtoull(f[6], &end, 16);
		if (end != NULL && *end == ',')
			sipk[1] = strtoull(end + 1, NULL, 16);
	}
	return 1;
}

/* Append index catalog row fields 4..7 ("<u>\0<descmask>\0<kind>\0
 * <k0>,<k1>", the last in hex) for `rt` at vbuf + off; returns the new
 * length.  The caller sized vbuf for them (64 bytes). */
static size_t
xs_idxcat_tail(char *vbuf, size_t cap, size_t off, const struct xs_idx_rt *rt)
{
	off += (size_t)snprintf(vbuf + off, cap - off, "%c",
	    rt->unique ? '1' : '0');
	off++;
	off += (size_t)snprintf(vbuf + off, cap - off, "%u",
	    (unsigned)rt->descmask);
	off++;
	off += (size_t)snprintf(vbuf + off, cap - off, "%c", rt->kind);
	off++;
	off += (size_t)snprintf(vbuf + off, cap - off, "%llx,%llx",
	    (unsigned long long)rt->k0, (unsigned long long)rt->k1);
	return off;
}

/* Load every persisted (non-tombstoned) index-catalog row for `bt` into
 * the in-process cache.  Called at open before rebuilding entries.
 * XTC_OK, or the XTC_E_* code of a read that failed: the cache then
 * holds only the definitions read before it. */
static int
xs_idxcat_load(bt_t *bt)
{
	bt_cursor_t *cur = NULL;
	int64_t last_rid = 0; int have_last = 0;
	struct { uint32_t id; char v[1100]; int vl; } *rows = NULL;
	int nrows = 0, i, rc;
	rows = malloc(sizeof *rows * XS_IDX_MAX);
	if (rows == NULL) return XTC_E_NOMEM;
	if ((rc = xs_cat_open(bt, XS_IDXCAT_TABLEID, &cur)) != XTC_OK) {
		free(rows);
		return rc == XTC_E_NOTFOUND ? XTC_OK : rc;
	}
	for (;;) {
		const void *k = NULL, *vv = NULL; uint16_t klen = 0, vl = 0;
		const uint8_t *kb, *vb; int64_t rid;
		rc = xs_cat_next(cur, &k, &klen, &vv, &vl);
		if (rc != XTC_OK || klen != XS_VKLEN) break;
		kb = (const uint8_t *)k; vb = (const uint8_t *)vv;
		if (dec_tableid(kb) != XS_IDXCAT_TABLEID) break;
		rid = dec_rowid(kb);
		if (have_last && rid == last_rid) continue;   /* older version */
		last_rid = rid; have_last = 1;
		if (rid <= 0) continue;          /* the high-water row */
		if (vl < 1 || (vb[0] & XS_F_DELETED)) continue;  /* dropped */
		if (nrows >= XS_IDX_MAX || vl - 1 >= (int)sizeof rows[0].v) continue;
		rows[nrows].id = (uint32_t)rid;
		memcpy(rows[nrows].v, vb + 1, (size_t)(vl - 1));
		rows[nrows].v[vl - 1] = '\0';
		rows[nrows].vl = vl - 1;
		nrows++;
	}
	bt_cursor_close(cur);
	if (rc != XTC_OK && rc != XTC_E_NOTFOUND) {
		/* A definition not loaded would not be maintained or
		 * enforced: the caller must not run on a part of the list. */
		free(rows);
		return rc;
	}
	/* Resolve outside the cursor: cache add reads the table catalog. */
	for (i = 0; i < nrows; i++) {
		const char *nm, *tb, *cl;
		int uq, nc;
		uint32_t dm;
		uint64_t sk[2];
		char kd, col[XS_IDX_NCOL][64];
		if (!xs_idxcat_parse(rows[i].v, rows[i].vl, &nm, &tb, &cl, &uq,
		        &dm, &kd, sk))
			continue;
		nc = xs_split_cols(cl, col);
		if (nc > 0 && (rc = xs_idx_cache_add(bt, rows[i].id, nm, tb, col,
		    nc, dm, uq, kd, sk)) != XTC_OK)
			break;
	}
	free(rows);
	return rc == XTC_E_NOTFOUND ? XTC_OK : rc;
}

/* Delete every index entry under `idx_id` (a range over the [0xFE][id]
 * prefix).  Used by DROP INDEX and before a full rebuild. */
static void
xs_idx_clear(bt_t *bt, uint32_t idx_id)
{
	uint8_t prefix[5];
	/* Bounded batch of keys.  KMAX is the B-tree's absolute key cap
	 * (BT_MAX_KEY; bt_max_key() <= 1024 on every page size), so every
	 * index entry fits.  Heap, not stack: this runs on fiber stacks. */
	enum { KMAX = XS_IDX_KMAX, BATCH = 32 };
	uint8_t (*dead)[KMAX];
	uint16_t deadlen[BATCH];
	int rc = XTC_OK;
	dead = malloc(sizeof *dead * BATCH);
	if (dead == NULL) {
		xtc_atomic_store_i32(&g_idx_taint, 1, xtc_mo_release);
		return;
	}
	(void)xs_idx_prefix(idx_id, prefix);
	for (;;) {
		bt_cursor_t *cur = NULL;
		int ndead = 0, j;
		if ((rc = xs_rd_count(bt_cursor_open(bt, prefix, 5, &cur))) !=
		    XTC_OK)
			break;
		for (;;) {
			const void *k = NULL, *vv = NULL; uint16_t klen = 0, vl = 0;
			rc = xs_rd_count(bt_cursor_next(cur, &k, &klen, &vv, &vl));
			if (rc != XTC_OK) break;
			if (klen < 5 || memcmp(k, prefix, 5) != 0) break;   /* past this index */
			if (klen > KMAX) continue;   /* unreachable: keys <= BT_MAX_KEY */
			memcpy(dead[ndead], k, klen); deadlen[ndead] = klen;
			if (++ndead >= BATCH) break;
		}
		bt_cursor_close(cur);
		for (j = 0; j < ndead; j++)
			(void)bt_delete(bt, dead[j], deadlen[j]);
		if (ndead < BATCH) break;
	}
	/* Entries left behind are stale (readers check each against its
	 * row), but no reap knows them: do not trust the indexes. */
	if (rc != XTC_OK && rc != XTC_E_NOTFOUND)
		xtc_atomic_store_i32(&g_idx_taint, 1, xtc_mo_release);
	free(dead);
}

/* A built key in a growable byte arena (offset + length). */
struct xs_kbuf {
	uint8_t  *bytes;
	size_t    used, cap;
	uint32_t *off;
	uint16_t *len;
	size_t    n, ncap;
};

static int
xs_kbuf_add(struct xs_kbuf *kb, const uint8_t *k, int klen)
{
	if (kb->used + (size_t)klen > kb->cap) {
		size_t nc = kb->cap ? kb->cap * 2 : 65536;
		uint8_t *nb;
		while (nc < kb->used + (size_t)klen) nc *= 2;
		nb = realloc(kb->bytes, nc);
		if (nb == NULL) return -1;
		kb->bytes = nb; kb->cap = nc;
	}
	if (kb->n == kb->ncap) {
		size_t nc = kb->ncap ? kb->ncap * 2 : 1024;
		uint32_t *no = realloc(kb->off, nc * sizeof *no);
		uint16_t *nl;
		if (no == NULL) return -1;
		kb->off = no;
		nl = realloc(kb->len, nc * sizeof *nl);
		if (nl == NULL) return -1;
		kb->len = nl; kb->ncap = nc;
	}
	memcpy(kb->bytes + kb->used, k, (size_t)klen);
	kb->off[kb->n] = (uint32_t)kb->used;
	kb->len[kb->n] = (uint16_t)klen;
	kb->used += (size_t)klen;
	kb->n++;
	return 0;
}

static _Thread_local const struct xs_kbuf *g_kbuf_sort;   /* qsort context */
static int
xs_kbuf_cmp(const void *pa, const void *pb)
{
	const struct xs_kbuf *kb = g_kbuf_sort;
	size_t a = *(const size_t *)pa, b = *(const size_t *)pb;
	uint16_t la = kb->len[a], lb = kb->len[b];
	int r = memcmp(kb->bytes + kb->off[a], kb->bytes + kb->off[b],
	    la < lb ? la : lb);
	if (r) return r;
	return (int)la - (int)lb;
}

/*
 * Build (or rebuild) one index's entries from the base table's live
 * rows: scan once computing each row's key, then insert the keys in
 * sorted order (sequential leaf fills, no cursor open while inserting).
 * For a UNIQUE index, two live rows with the same non-NULL key tuple
 * fail the build: returns -1 (CREATE UNIQUE INDEX over duplicates is a
 * UNIQUE constraint error in SQLite) and inserts nothing.  A build that
 * cannot see every row (the scan failed a read, or memory ran out)
 * also inserts nothing and returns the XTC_E_* code, which is below -1:
 * an index built from part of the table would answer short.  0 on
 * success.
 */
static int
xs_idx_build_one(bt_t *bt, const struct xs_idx_ent *e)
{
	struct xs_kbuf kb;
	xstore_scan_t *s;
	int64_t rowid; const uint8_t *rec; int reclen;
	uint8_t key[XS_IDX_KMAX];
	size_t *ord = NULL, i;
	int rc = 0, klen, isnull, step;
	char *nullf = NULL;
	if (e->rt.tableid == 0) return 0;
	memset(&kb, 0, sizeof kb);
	s = xstore_scan_open(bt, e->table, 0, 0, 0, 0, 0);
	if (s == NULL) {  /* the table went away, no memory, or unread */
		rc = xstore_table_id(bt, e->table, NULL);
		return rc < 0 ? rc : rc > 0 ? XTC_E_NOMEM : XTC_E_NOTFOUND;
	}
	while ((step = xstore_scan_next(s, &rowid, &rec, &reclen)) == 1) {
		klen = xs_idx_rowkey(&e->rt, rec, reclen, rowid, 1, key,
		    (int)sizeof key, &isnull);
		if (klen < 0) { /* a page too small for the key */
			rc = XTC_E_INTERNAL;
			break;
		}
		/* Carry the NULL flag in the key's slot via a side array. */
		if (xs_kbuf_add(&kb, key, klen) != 0) { rc = XTC_E_NOMEM; break; }
		if (e->rt.unique) {
			char *nf = realloc(nullf, kb.ncap);
			if (nf == NULL) { rc = XTC_E_NOMEM; break; }
			nullf = nf;
			nullf[kb.n - 1] = (char)isnull;
		}
	}
	/* The scan's own error has no code of its own: a failed read. */
	if (rc == 0 && step < 0)
		rc = XTC_E_IO;
	xstore_scan_close(s);
	if (rc == 0 && kb.n > 0) {
		ord = malloc(kb.n * sizeof *ord);
		if (ord == NULL) rc = XTC_E_NOMEM;
	}
	if (rc == 0 && kb.n > 0) {
		for (i = 0; i < kb.n; i++) ord[i] = i;
		g_kbuf_sort = &kb;
		qsort(ord, kb.n, sizeof *ord, xs_kbuf_cmp);
		g_kbuf_sort = NULL;
		if (e->rt.unique)
			for (i = 1; i < kb.n; i++) {
				size_t a = ord[i - 1], b = ord[i];
				if (nullf[a] || nullf[b]) continue;
				if (kb.len[a] == kb.len[b] &&
				    memcmp(kb.bytes + kb.off[a], kb.bytes + kb.off[b],
				    (size_t)kb.len[a] - 8) == 0) {
					rc = -1;          /* duplicate key tuple */
					break;
				}
			}
		for (i = 0; rc == 0 && i < kb.n; i++)
			(void)xs_idx_put(bt, kb.bytes + kb.off[ord[i]],
			    kb.len[ord[i]]);
	}
	free(ord); free(nullf);
	free(kb.bytes); free(kb.off); free(kb.len);
	return rc;
}

/*
 * Rebuild work: the indexes are independent (each its own key range), so
 * workers on the blocking pool build them one at a time -- the open then
 * costs about the largest index instead of the sum (TPC-C W=10: 10
 * indexes, two of them 5.9M entries each).  Clearing the old entries runs
 * first and serially: deletes merge leaves under the tree's one SMO lock,
 * and 16 clearing threads took 27 s where one takes 8 s.  Under a
 * simulation build the pool would run on real threads outside the
 * schedule, so the work runs inline there (deterministic), as it does
 * with one index or one CPU.
 */
struct xs_rb_job {
	bt_t              *bt;
	struct xs_idx_ent *e;
	int                n;
	int next, left;
};

/* Take index `idx_id` out of the in-process cache (its catalog row, if
 * any, is the caller's to handle). */
static void
xs_idx_cache_drop(bt_t *bt, uint32_t idx_id)
{
	int i;
	xs_cat_lock();
	for (i = 0; i < g_idx_n; i++)
		if (g_idx[i].bt == bt && g_idx[i].rt.idx_id == idx_id) {
			g_idx[i] = g_idx[g_idx_n - 1];
			XS_IDX_N_SET(g_idx_n - 1);
			XS_CATC_BUMP();
			break;
		}
	xs_cat_unlock();
}

/* Index `idx_id` could not be rebuilt (code rc): it misses entries for
 * the rest of the run.  The planner and the index scan skip it, and a
 * UNIQUE probe through it fails the write; the taint rebuilds it at the
 * next open, and a later rebuild of this run replaces the entry. */
static void
xs_idx_mark_broken(bt_t *bt, uint32_t idx_id, int rc)
{
	int i;
	xs_cat_lock();
	for (i = 0; i < g_idx_n; i++)
		if (g_idx[i].bt == bt && g_idx[i].rt.idx_id == idx_id) {
			g_idx[i].rt.broken = rc;
			XS_CATC_BUMP();
			break;
		}
	xs_cat_unlock();
	xs_cat_gen_bump();           /* re-plan without it */
}

static int
xs_rb_worker(void *arg)
{
	struct xs_rb_job *j = arg;
	int i, rc;
	while ((i = xtc_atomic_fetch_add_i32(&j->next, 1, xtc_mo_relaxed)) <
	    j->n) {
#if XTC_DST_BUG(XTC_DST_BUG_IDXSKIP)
		if (j->n > 1 && i == j->n - 1)
			continue;               /* planted: one index never rebuilt */
#endif
		/* A UNIQUE duplicate builds nothing; a failed read or no
		 * memory builds nothing either.  Either way the index is
		 * incomplete: not used this run, not trusted at the next
		 * open. */
		rc = xs_idx_build_one(j->bt, &j->e[i]);
		if (rc != 0) {
			xtc_atomic_store_i32(&g_idx_taint, 1, xtc_mo_release);
			xs_idx_mark_broken(j->bt, j->e[i].rt.idx_id,
			    rc < -1 ? rc : XTC_E_INTERNAL);
		}
	}
	xtc_atomic_fetch_sub_i32(&j->left, 1, xtc_mo_acq_rel);
	return 0;
}

/* Load the persisted catalog and rebuild every index of `bt` from the
 * base rows, on up to `workers` threads.  clear = 0 when the index
 * namespace is known empty (a tree rebuilt from the log on a fresh page
 * file).  XTC_OK, or the error of a catalog that could not be read (no
 * index is rebuilt then). */
static int
xs_idx_rebuild(bt_t *bt, int clear, int workers)
{
	struct xs_rb_job j;
	int i, nw, rc;
	memset(&j, 0, sizeof j);
	xs_idx_dead_forget(bt);      /* every entry is rebuilt from the base */
	xtc_atomic_store_u64(&g_idx_garbage, 0, xtc_mo_relaxed);
	/* Refresh the cache from the persisted catalog. */
	if ((rc = xs_idxcat_load(bt)) != XTC_OK) {
		xtc_atomic_store_i32(&g_idx_taint, 1, xtc_mo_release);
		return rc;
	}
	j.e = malloc(sizeof *j.e * XS_IDX_MAX);
	if (j.e == NULL) {
		xtc_atomic_store_i32(&g_idx_taint, 1, xtc_mo_release);
		return XTC_E_NOMEM;
	}
	j.bt = bt;
	xs_cat_lock();
	for (i = 0; i < g_idx_n && j.n < XS_IDX_MAX; i++)
		if (g_idx[i].bt == bt) j.e[j.n++] = g_idx[i];
	xs_cat_unlock();
	for (i = 0; clear && i < j.n; i++)
		xs_idx_clear(bt, j.e[i].rt.idx_id);
	xtc_atomic_fetch_add_u64(&g_idx_rebuilds, 1, xtc_mo_relaxed);
	nw = strcmp(xtc_io_backend_name(), "sim") == 0 ? 1 : workers;
	if (nw > j.n) nw = j.n;
	if (nw < 1) nw = 1;
	xtc_atomic_store_i32(&j.left, nw, xtc_mo_seq_cst);
	for (i = 1; i < nw; i++)
		if (xtc_blocking_submit(xs_rb_worker, &j) != XTC_OK)
			xtc_atomic_fetch_sub_i32(&j.left, 1,
			    xtc_mo_seq_cst); /* this thread does its share */
	(void)xs_rb_worker(&j);
	while (xtc_atomic_load_i32(&j.left, xtc_mo_acquire) > 0) {
		if (xtc_pid_is_none(xtc_self()))
			(void)xtc_thrd_sleep_ns(1000LL * 1000);
		else
			(void)xtc_proc_sleep(1000LL * 1000);
	}
	free(j.e);
	return XTC_OK;
}

int
xstore_index_rebuild_all(bt_t *bt)
{
	if (bt == NULL) return 0;
	return xs_idx_rebuild(bt, 1, 1);
}

uint64_t
xstore_index_trust_tag(bt_t *bt)
{
	int i, unresolved = 0;
	if (xtc_atomic_load_i32(&g_idx_taint, xtc_mo_acquire))
		return 0;
	xs_cat_lock();
	for (i = 0; i < g_idx_n; i++)
		if (g_idx[i].bt == bt && (g_idx[i].rt.tableid == 0 ||
		    g_idx[i].rt.broken != 0))
			unresolved = 1;            /* not complete this run */
	xs_cat_unlock();
	return unresolved ? 0 : XS_IDX_TRUST_TAG;
}

int
xstore_index_open(bt_t *bt, uint64_t tag, uint64_t garbage, int fresh,
    int workers)
{
	int rc;
	if (bt == NULL) return 0;
	xtc_atomic_store_i32(&g_idx_taint, 0, xtc_mo_release);
	if (!fresh && tag == XS_IDX_TRUST_TAG && garbage < XS_IDX_GARBAGE_MAX) {
		xs_idx_dead_forget(bt);
		xtc_atomic_store_u64(&g_idx_garbage, garbage, xtc_mo_relaxed);
		/* The entries are complete: trust them. */
		if ((rc = xs_idxcat_load(bt)) != XTC_OK)
			return rc;
		/* Loading may taint (an unresolvable index): rebuild then. */
		if (!xtc_atomic_load_i32(&g_idx_taint, xtc_mo_acquire))
			return 1;
		xtc_atomic_store_i32(&g_idx_taint, 0, xtc_mo_release);
	}
	rc = xs_idx_rebuild(bt, !fresh, workers);
	return rc != XTC_OK ? rc : 0;
}

uint64_t
xstore_index_close(bt_t *bt)
{
	if (bt != NULL)
		xs_idx_drain(bt);
	return xtc_atomic_load_u64(&g_idx_garbage, xtc_mo_relaxed);
}

uint64_t
xstore_index_rebuild_count(void)
{
	return xtc_atomic_load_u64(&g_idx_rebuilds, xtc_mo_relaxed);
}

/*
 * Check every cached index of `bt` against the base: *missing counts live
 * rows whose newest committed version has no entry (a seek or a UNIQUE
 * check would miss the row), *stale counts entries the row's newest
 * committed version does not carry (harmless: readers filter them).
 * Run quiesced (tests, tools).  Returns the number of indexes checked,
 * or the XTC_E_* code of a read that failed (the counts are then short).
 */
int
xstore_index_check(bt_t *bt, uint64_t *missing, uint64_t *stale)
{
	struct xs_idx_ent *snap;
	uint8_t key[XS_IDX_KMAX], kk[XS_IDX_KMAX], pfx[5];
	uint8_t *rec = NULL;
	int ns = 0, i, kl, rl, step = 0, rc, err = 0;
	uint64_t miss = 0, st = 0;
	if (bt == NULL) return 0;
	snap = malloc(sizeof *snap * XS_IDX_MAX);
	rec = malloc(XS_VMAX);
	if (snap == NULL || rec == NULL) { free(snap); free(rec); return 0; }
	xs_cat_lock();
	for (i = 0; i < g_idx_n && ns < XS_IDX_MAX; i++)
		if (g_idx[i].bt == bt && g_idx[i].rt.tableid != 0)
			snap[ns++] = g_idx[i];
	xs_cat_unlock();
	for (i = 0; i < ns; i++) {
		xstore_scan_t *s = xstore_scan_open(bt, snap[i].table, 0, 0, 0, 0, 0);
		bt_cursor_t *cur = NULL;
		int64_t rowid; const uint8_t *r; int rlen;
		uint16_t vl;
		if (s == NULL) err = XTC_E_NOMEM;
		while (s != NULL &&
		    (step = xstore_scan_next(s, &rowid, &r, &rlen)) == 1) {
			kl = xs_idx_rowkey(&snap[i].rt, r, rlen, rowid, 1, key,
			    (int)sizeof key, NULL);
			rc = kl < 0 ? XTC_E_NOTFOUND :
			    bt_lookup(bt, key, (uint16_t)kl, kk, 0, &vl);
			if (rc == XTC_E_NOTFOUND)
				miss++;
			else if (rc != XTC_OK)
				err = rc;     /* unread: not "missing" */
		}
		if (step < 0) err = XTC_E_IO;
		xstore_scan_close(s);
		(void)xs_idx_prefix(snap[i].rt.idx_id, pfx);
		if ((rc = bt_cursor_open(bt, pfx, 5, &cur)) != XTC_OK) {
			err = rc;
			continue;
		}
		for (;;) {
			const void *k = NULL, *vv = NULL; uint16_t klen = 0, vlen = 0;
			int64_t rid;
			rc = bt_cursor_next(cur, &k, &klen, &vv, &vlen);
			if (rc != XTC_OK && rc != XTC_E_NOTFOUND) err = rc;
			if (rc != XTC_OK || klen < 13 || memcmp(k, pfx, 5) != 0)
				break;
			memcpy(key, k, klen);
			kl = klen;
			rid = (int64_t)(xs_get_be64(key + kl - 8) ^ 0x8000000000000000ull);
			bt_cursor_park(cur);
			rl = xs_read_rec(bt, snap[i].rt.tableid, rid, rec, XS_VMAX);
			if (rl < 0) err = rl;
			if (rl <= 0 || xs_idx_rowkey(&snap[i].rt, rec, rl, rid, 1, kk,
			    (int)sizeof kk, NULL) != kl || memcmp(kk, key, (size_t)kl) != 0)
				st++;
			if ((rc = bt_cursor_resume(cur)) != XTC_OK) {
				err = rc;
				break;
			}
		}
		bt_cursor_close(cur);
	}
	free(snap); free(rec);
	if (missing) *missing = miss;
	if (stale) *stale = st;
	return err != 0 ? err : ns;
}

int
xstore_create_index_ex(struct xsql *db, const char *name, const char *table,
    const char *const *cols, int ncol, uint32_t descmask, int unique, int kind)
{
	bt_t *bt = xstore_bt_of(db);
	struct xs_idx_ent e;
	uint32_t tableid = 0, idx_id;
	uint64_t sipk[2];
	int i, exists = 0, rc;
	char vbuf[1100];
	size_t off = 0, cl = 0;
	if (bt == NULL || name == NULL || table == NULL || cols == NULL ||
	    ncol < 1 || ncol > XS_IDX_NCOL) return 0;
	if ((rc = xstore_table_id(bt, table, &tableid)) <= 0)
		return rc < 0 ? rc : 0;   /* unread / unknown table */
	memset(&e, 0, sizeof e);
	for (i = 0; i < ncol; i++) {
		if (strlen(cols[i]) >= sizeof e.col[0]) return 0;
		snprintf(e.col[i], sizeof e.col[i], "%s", cols[i]);
		cl += strlen(cols[i]) + 1;
	}
	if ((rc = xs_idx_resolve(bt, table, e.col, ncol, e.rt.pay)) != 0)
		return rc < -1 ? rc : 0;        /* unread / unknown column */

	/* Idempotent by name: if an index of this name already exists, keep it. */
	xs_cat_lock();
	for (i = 0; i < g_idx_n; i++)
		if (g_idx[i].bt == bt && strcmp(g_idx[i].name, name) == 0) { exists = 1; break; }
	xs_cat_unlock();
	if (exists) return 1;

	if ((rc = xs_idxcat_max_id(bt, &idx_id)) != XTC_OK)
		return rc;                      /* the ids in use are unknown */
	idx_id++;
	if (idx_id == 0u) idx_id = 1u;

	/* Persist "<name>\0<table>\0<c1,c2,..>\0<u>\0<descmask>\0<kind>\0
	 * <k0>,<k1>" as a WAL-durable version row under XS_IDXCAT_TABLEID, so
	 * the definition survives a crash (its entries are rebuilt from base
	 * rows at open). */
	if (strlen(name) + strlen(table) + cl + 64 > sizeof vbuf)
		return 0;
	off += (size_t)snprintf(vbuf + off, sizeof vbuf - off, "%s", name) + 1;
	off += (size_t)snprintf(vbuf + off, sizeof vbuf - off, "%s", table) + 1;
	for (i = 0; i < ncol; i++)
		off += (size_t)snprintf(vbuf + off, sizeof vbuf - off, "%s%s",
		    i ? "," : "", cols[i]);
	off++;
	/* The SipHash key of truncated entries: random per index (a client
	 * cannot aim keys at one hash), virtualized under the simulator. */
	sipk[0] = xtc_rand_u64();
	sipk[1] = xtc_rand_u64();
	e.rt.unique = unique;
	e.rt.descmask = descmask;
	e.rt.kind = kind ? (char)kind : 'i';
	e.rt.k0 = sipk[0];
	e.rt.k1 = sipk[1];
	off = xs_idxcat_tail(vbuf, sizeof vbuf, off, &e.rt);
	/* Enter the cache BEFORE the build so a commit applied from here on
	 * maintains the index; the barrier below waits out the commits in
	 * flight (which may have missed it), and the build scans after it. */
	/* The resolve reads the catalog again when its cache is full: an
	 * index not in the cache would be built from nothing, persisted, and
	 * trusted empty at the next open. */
	if ((rc = xs_idx_cache_add(bt, idx_id, name, table, e.col, ncol,
	    descmask, unique, kind ? (char)kind : 'i', sipk)) != XTC_OK)
		return rc;
	xtc_atomic_fetch_add_u64(&g_idx_gen, 1, xtc_mo_release);
	xs_cat_gen_bump();
	/* A store the engine has stopped never opens its gate: fail. */
	if (!xs_quiesce_begin(XS_QB_STOP)) {
		xs_idx_cache_drop(bt, idx_id);
		return XTC_E_STATE;
	}
	xstore_quiesce_end();
	xs_cat_lock();
	for (i = 0; i < g_idx_n; i++)
		if (g_idx[i].bt == bt && g_idx[i].rt.idx_id == idx_id) { e = g_idx[i]; break; }
	xs_cat_unlock();
	/* A constraint index (CREATE TABLE's autoindex) is persisted before
	 * its build: a build that fails leaves it durable and broken, so it
	 * refuses writes and the next open rebuilds it.  Removing it instead
	 * would leave the table without its constraint whenever the clean-up
	 * DROP could not run (a crash, or a second failed read). */
	if (kind != 0 && kind != 'i') {
		if (xs_put(bt, XS_IDXCAT_TABLEID, (int64_t)idx_id, vbuf,
		    (int)off, 0) != SX_OK) {
			xs_idx_cache_drop(bt, idx_id);
			return 0;
		}
		rc = xs_idx_build_one(bt, &e);
		if (rc != 0) {
			xtc_atomic_store_i32(&g_idx_taint, 1, xtc_mo_release);
			xs_idx_mark_broken(bt, idx_id, rc < -1 ? rc : XTC_E_INTERNAL);
		}
		return rc != 0 ? rc : 1;
	}
	rc = xs_idx_build_one(bt, &e);
	if (rc != 0) {
		/* Duplicates (SQLite refuses the index), or the table could not
		 * be read whole: undo, so no index is left behind. */
		xs_cat_lock();
		for (i = 0; i < g_idx_n; i++)
			if (g_idx[i].bt == bt && g_idx[i].rt.idx_id == idx_id) {
				g_idx[i] = g_idx[g_idx_n - 1];
				XS_IDX_N_SET(g_idx_n - 1);
				XS_CATC_BUMP();
				break;
			}
		xs_cat_unlock();
		xs_idx_clear(bt, idx_id);
		return rc;
	}
	if (xs_put(bt, XS_IDXCAT_TABLEID, (int64_t)idx_id, vbuf, (int)off, 0) != SX_OK)
		return 0;
	return 1;
}

int
xstore_create_index(struct xsql *db, const char *name, const char *table,
    const char *col, int unique)
{
	const char *cols[1];
	cols[0] = col;
	return xstore_create_index_ex(db, name, table, cols, 1, 0, unique, 'i') == 1;
}

int
xstore_drop_index(struct xsql *db, const char *name)
{
	bt_t *bt = xstore_bt_of(db);
	uint32_t idx_id = 0;
	int i, found = 0, rc;
	uint8_t vbuf[8];
	xs_cat_gen_bump();
	if (bt == NULL || name == NULL) return 0;
	xs_cat_lock();
	for (i = 0; i < g_idx_n; i++)
		if (g_idx[i].bt == bt && strcmp(g_idx[i].name, name) == 0) {
			if (g_idx[i].rt.kind != 'i') { found = -1; break; }
			idx_id = g_idx[i].rt.idx_id; found = 1;
			break;
		}
	xs_cat_unlock();
	if (found <= 0) return found;   /* unknown (0) / constraint index (-1) */
	/* The mark first, before anything is removed: a mark that cannot
	 * be read or written fails the DROP with the index whole.  Clearing
	 * the entries of an index whose catalog row stays live would leave
	 * an empty index that the next open trusts. */
	if ((rc = xs_hw_raise(bt, XS_IDXCAT_TABLEID, idx_id)) != SX_OK)
		return rc < -1 ? rc : XTC_E_IO;
	xs_cat_lock();
	for (i = 0; i < g_idx_n; i++)
		if (g_idx[i].bt == bt && g_idx[i].rt.idx_id == idx_id) {
			if (i != g_idx_n - 1) g_idx[i] = g_idx[g_idx_n - 1];
			XS_IDX_N_SET(g_idx_n - 1);
			XS_CATC_BUMP();
			break;
		}
	xs_cat_unlock();
	vbuf[0] = 0;
	if (xs_drop_gap("xstore.drop.index_gap") ||
	    xs_put(bt, XS_IDXCAT_TABLEID, (int64_t)idx_id, vbuf, 1, 1) != SX_OK)
		xtc_atomic_store_i32(&g_idx_taint, 1, xtc_mo_release);
	xs_idx_clear(bt, idx_id);
	return 1;
}

/* The largest index id of `table`'s indexes, or 0 if it has none. */
static uint32_t
xs_idx_table_top(bt_t *bt, const char *table)
{
	uint32_t top = 0;
	int i;
	xs_cat_lock();
	for (i = 0; i < g_idx_n; i++)
		if (g_idx[i].bt == bt && strcmp(g_idx[i].table, table) == 0 &&
		    g_idx[i].rt.idx_id > top)
			top = g_idx[i].rt.idx_id;
	xs_cat_unlock();
	return top;
}

/* Drop every index of `table` (DROP TABLE), whose caller has raised the
 * index id mark past them (xs_idx_table_top). */
static void
xs_idx_drop_table(bt_t *bt, const char *table)
{
	uint32_t ids[XS_IDX_MAX];
	uint8_t vbuf[8];
	int n = 0, i, marked;
	xs_cat_lock();
	for (i = 0; i < g_idx_n; ) {
		if (g_idx[i].bt == bt && strcmp(g_idx[i].table, table) == 0) {
			ids[n++] = g_idx[i].rt.idx_id;
			g_idx[i] = g_idx[g_idx_n - 1];
			XS_IDX_N_SET(g_idx_n - 1);
			XS_CATC_BUMP();
		} else {
			i++;
		}
	}
	xs_cat_unlock();
	vbuf[0] = 0;
	/* A tombstone that cannot be written (a fail-stopped log, so the
	 * store takes no more writes) leaves its catalog row live: taint,
	 * so the next open rebuilds rather than trusts it. */
	marked = !xs_drop_gap("xstore.drop.index_gap");
	for (i = 0; i < n; i++) {
		if (!marked || xs_put(bt, XS_IDXCAT_TABLEID, (int64_t)ids[i], vbuf,
		    1, 1) != SX_OK)
			xtc_atomic_store_i32(&g_idx_taint, 1, xtc_mo_release);
		xs_idx_clear(bt, ids[i]);
	}
}

/* Re-point `table`'s indexes at its new name (ALTER TABLE RENAME): the
 * cache entries and their persisted catalog rows. */
static void
xs_idx_rename_table(bt_t *bt, const char *oldname, const char *newname)
{
	struct xs_idx_ent *snap;
	int n = 0, i, c;
	snap = malloc(sizeof *snap * XS_IDX_MAX);
	if (snap == NULL) return;
	xs_cat_lock();
	for (i = 0; i < g_idx_n; i++)
		if (g_idx[i].bt == bt && strcmp(g_idx[i].table, oldname) == 0) {
			snprintf(g_idx[i].table, sizeof g_idx[i].table, "%s", newname);
			XS_CATC_BUMP();
			snap[n++] = g_idx[i];
		}
	xs_cat_unlock();
	for (i = 0; i < n; i++) {
		char vbuf[1100];
		size_t off = 0;
		off += (size_t)snprintf(vbuf + off, sizeof vbuf - off, "%s", snap[i].name) + 1;
		off += (size_t)snprintf(vbuf + off, sizeof vbuf - off, "%s", newname) + 1;
		for (c = 0; c < snap[i].rt.ncol; c++)
			off += (size_t)snprintf(vbuf + off, sizeof vbuf - off, "%s%s",
			    c ? "," : "", snap[i].col[c]);
		off++;
		off = xs_idxcat_tail(vbuf, sizeof vbuf, off, &snap[i].rt);
		(void)xs_put(bt, XS_IDXCAT_TABLEID, (int64_t)snap[i].rt.idx_id,
		    vbuf, (int)off, 0);
	}
	free(snap);
}

int
xstore_index_list(bt_t *bt, const char *table, xstore_index_info_t *out,
    int cap)
{
	/* This thread's last few answers, valid while g_catc_gen is
	 * unchanged (every index create / drop / rebuild bumps it): the
	 * planner asks per write statement, and taking the catalog lock for
	 * it queued every connection of a busy server on that lock. */
	static _Thread_local struct {
		uint64_t gen; bt_t *bt; char table[64]; int n;
		xstore_index_info_t ix[8];
	} tc[4];
	static _Thread_local unsigned tnext;
	uint64_t g;
	int i, c, n = 0, k;
	if (bt == NULL || table == NULL || out == NULL) return 0;
	g = xtc_atomic_load_u64(&g_catc_gen, xtc_mo_acquire);
	for (k = 0; k < 4; k++)
		if (tc[k].gen == g && tc[k].bt == bt && tc[k].n <= cap &&
		    strcasecmp(tc[k].table, table) == 0) {
			memcpy(out, tc[k].ix, sizeof out[0] * (size_t)tc[k].n);
			return tc[k].n;
		}
	xs_cat_lock();
	g = xtc_atomic_load_u64(&g_catc_gen, xtc_mo_relaxed);
	for (i = 0; i < g_idx_n && n < cap; i++) {
		if (g_idx[i].bt != bt || strcasecmp(g_idx[i].table, table) != 0 ||
		    g_idx[i].rt.tableid == 0 || g_idx[i].rt.broken != 0)
			continue;
		memset(&out[n], 0, sizeof out[n]);
		out[n].idx_id = g_idx[i].rt.idx_id;
		snprintf(out[n].name, sizeof out[n].name, "%s", g_idx[i].name);
		out[n].ncol = g_idx[i].rt.ncol;
		for (c = 0; c < g_idx[i].rt.ncol; c++)
			snprintf(out[n].col[c], sizeof out[n].col[c], "%s",
			    g_idx[i].col[c]);
		out[n].descmask = g_idx[i].rt.descmask;
		out[n].unique = g_idx[i].rt.unique;
		out[n].kind = g_idx[i].rt.kind;
		n++;
	}
	xs_cat_unlock();
	if (n <= 8 && strlen(table) < sizeof tc[0].table) {
		k = (int)(tnext++ % 4u);
		tc[k].gen = g; tc[k].bt = bt; tc[k].n = n;
		snprintf(tc[k].table, sizeof tc[k].table, "%s", table);
		memcpy(tc[k].ix, out, sizeof out[0] * (size_t)n);
	}
	return n;
}

static int
xs_idx_get_rt(bt_t *bt, uint32_t idx_id, struct xs_idx_rt *rt)
{
	/* This thread's last few, valid while g_catc_gen is unchanged: the
	 * reap asks once per queued entry, which took the catalog lock once
	 * per UPDATE of an indexed row. */
	static _Thread_local struct {
		uint64_t gen; bt_t *bt; struct xs_idx_rt rt;
	} tc[8];
	static _Thread_local unsigned tnext;
	uint64_t g = xtc_atomic_load_u64(&g_catc_gen, xtc_mo_acquire);
	int i, r = 0;
	for (i = 0; i < 8; i++)
		if (tc[i].gen == g && tc[i].bt == bt && tc[i].rt.idx_id == idx_id) {
			*rt = tc[i].rt;
			return 1;
		}
	xs_cat_lock();
	g = xtc_atomic_load_u64(&g_catc_gen, xtc_mo_relaxed);
	for (i = 0; i < g_idx_n; i++)
		if (g_idx[i].bt == bt && g_idx[i].rt.idx_id == idx_id) {
			*rt = g_idx[i].rt; r = 1; break;
		}
	xs_cat_unlock();
	if (r) {
		i = (int)(tnext++ % 8u);
		tc[i].gen = g; tc[i].bt = bt; tc[i].rt = *rt;
	}
	return r;
}

/* Find index `idx_id` of `bt`; copies it to *e.  Returns 1 / 0.  Read
 * per index seek: served from this thread's copies while g_catc_gen is
 * unchanged (see g_catc_gen). */
static int
xs_idx_get(bt_t *bt, uint32_t idx_id, struct xs_idx_ent *e)
{
	static _Thread_local struct { uint64_t gen; struct xs_idx_ent e; } tc[8];
	static _Thread_local unsigned tnext;
	uint64_t g = xtc_atomic_load_u64(&g_catc_gen, xtc_mo_acquire);
	int i, r = 0;
	for (i = 0; i < 8; i++)
		if (tc[i].gen == g && tc[i].e.bt == bt && tc[i].e.rt.idx_id == idx_id) {
			*e = tc[i].e;
			return 1;
		}
	xs_cat_lock();
	g = xtc_atomic_load_u64(&g_catc_gen, xtc_mo_relaxed);
	for (i = 0; i < g_idx_n; i++)
		if (g_idx[i].bt == bt && g_idx[i].rt.idx_id == idx_id) {
			*e = g_idx[i]; r = 1; break;
		}
	xs_cat_unlock();
	if (r) {
		i = (int)(tnext++ % 8u);
		tc[i].gen = g;
		tc[i].e = *e;
	}
	return r;
}

int
xstore_index_lookup(bt_t *bt, const char *table, const char *col,
    uint32_t *idx_id)
{
	int i, r = 0;
	if (bt == NULL || table == NULL || col == NULL) return 0;
	xs_cat_lock();
	for (i = 0; i < g_idx_n; i++)
		if (g_idx[i].bt == bt && g_idx[i].rt.tableid != 0 &&
		    g_idx[i].rt.broken == 0 &&
		    strcmp(g_idx[i].table, table) == 0 &&
		    strcmp(g_idx[i].col[0], col) == 0 &&
		    !(g_idx[i].rt.descmask & 1u)) {
			if (idx_id) *idx_id = g_idx[i].rt.idx_id;
			r = 1; break;
		}
	xs_cat_unlock();
	return r;
}

/* Number of index seeks the planner has served (metric/test evidence
 * that the seek path -- not a full scan -- was taken). */
uint64_t
xstore_index_seek_count(void)
{
	return xtc_atomic_load_u64(&g_idx_seeks, xtc_mo_relaxed);
}

/* ---- UNIQUE enforcement ----------------------------------------- */


/* The payload of buffered entry `wi` (not a delete) into *rec and
 * *len, bringing a spilled payload back first.  XTC_OK, XTC_E_NOMEM, or
 * XTC_E_IO when the staging area cannot be read back (a read of the
 * index-read fault hook, xstore_index_read_fail_at, when spilled). */
static int
xs_wrec_payload(xstore_ctx_t *c, int wi, const uint8_t **rec, int *len)
{
	xs_wrec_t *w = &c->wbuf[wi];
	int rc;
	if (w->spilled && xs_rd_fault())
		return XTC_E_IO;
	rc = xs_wrec_ensure(c, w);
	if (rc != SX_OK)
		return rc == SX_NOMEM ? XTC_E_NOMEM : XTC_E_IO;
	*rec = w->data;
	*len = (int)w->len;
	return XTC_OK;
}

/* The row a UNIQUE check is for: its record and rowid. */
struct xs_urow {
	const uint8_t *rec;
	int reclen;
	int64_t rowid;
};

/* Is a candidate row (record crec/clen at crid, entry key ck/ckl without
 * the rowid) a duplicate of the checked row `w`, whose entry key is
 * key/klen?  The entries must match; a truncated match is a duplicate
 * only if the whole tuples match too (equal hashes prove nothing). */
static int
xs_uniq_same(const struct xs_idx_rt *rt, const uint8_t *ck, int ckl,
    const uint8_t *key, int klen, const uint8_t *crec, int clen, int64_t crid,
    const struct xs_urow *w)
{
	if (ckl != klen || memcmp(ck, key, (size_t)klen) != 0)
		return 0;
	if (!xs_idx_truncated(rt, klen + 8))
		return 1;
	return xs_idx_tuple_eq(rt, crec, clen, crid, w->rec, w->reclen,
	    w->rowid);
}

/* Record entry `wi`'s unique key tuples in the umap. */
static void
xs_umap_index(xstore_ctx_t *c, int wi)
{
	struct xs_idx_rt hits[16];
	uint8_t key[XS_IDX_KMAX];
	const uint8_t *rec;
	int nh, i, klen, isnull, rl = 0;
	xs_wrec_t *w = &c->wbuf[wi];
	if (w->deleted || !XS_IDX_ANY()) return;
	/* A spilled payload that cannot be read back is left out: the
	 * own-write UNIQUE check then walks the buffer (umap_bad). */
	if (w->spilled && (xs_row_fault() || xs_wrec_ensure(c, w) != SX_OK)) {
		c->umap_bad = 1;
		return;
	}
	nh = xs_idx_for_table(c->bt, w->tableid, hits, 16);
	rec = w->data; rl = (int)w->len;
	for (i = 0; i < nh; i++) {
		if (!hits[i].unique) continue;
		klen = xs_idx_rowkey(&hits[i], rec, rl, w->rowid, 0, key,
		    (int)sizeof key, &isnull);
		if (klen < 0 || isnull) continue;
		umap_put(c, xs_bytes_hash(key, klen), hits[i].idx_id, wi);
	}
}

/* Format "UNIQUE constraint failed: t.a, t.b" for index `e` into out. */
static void
xs_uniq_msg(const struct xs_idx_ent *e, char *out, size_t cap)
{
	size_t off;
	int i;
	off = (size_t)snprintf(out, cap, "UNIQUE constraint failed: ");
	for (i = 0; i < e->rt.ncol && off < cap; i++)
		off += (size_t)snprintf(out + off, cap - off, "%s%s.%s",
		    i ? ", " : "", e->table, e->col[i]);
}

/* Format the error of a UNIQUE probe of index `idx_id` that failed with
 * XTC_E_* code `rc`: "index error: cannot check UNIQUE t.a: <reason>".
 * The engine passes a message with the "index error" prefix to the
 * client unchanged. */
static void
xs_idx_err_msg(bt_t *bt, uint32_t idx_id, int rc, char *out, size_t cap)
{
	struct xs_idx_ent e;
	if (xs_idx_get(bt, idx_id, &e) && e.rt.broken != 0)
		snprintf(out, cap, "index error: index %.48s is incomplete "
		    "(%s); a restart rebuilds it", e.name,
		    xtc_strerror(e.rt.broken));
	else if (xs_idx_get(bt, idx_id, &e))
		snprintf(out, cap, "index error: cannot check UNIQUE %s.%s: %s",
		    e.table, e.col[0], xtc_strerror(rc));
	else
		snprintf(out, cap, "index error: cannot check UNIQUE: %s",
		    xtc_strerror(rc));
}

/* Does a live COMMITTED row other than `rowid` carry key tuple `key`
 * (no rowid suffix) in index `rt`?  The newest committed version of
 * each candidate decides; a candidate this transaction has a buffered
 * write for is skipped (`cx` non-NULL: the buffered state decides, and
 * the umap / statement check covers it).  Returns 1 (a duplicate), 0
 * (none), or a negative XTC_E_* code when the probe could not run to
 * the end of the key's entries: NB the caller must fail the write then,
 * since a probe that stopped early has not proven the key absent. */
static int
xs_uniq_committed_dup(bt_t *bt, const struct xs_idx_rt *rt, const uint8_t *key,
    int klen, const struct xs_urow *w, xstore_ctx_t *cx)
{
	int64_t rowid = w->rowid;
	bt_cursor_t *cur = NULL;
	uint8_t *rec = NULL, kk[XS_IDX_KMAX];
	int dup = 0, rc;
	if (rt->broken != 0)
		return rt->broken;   /* misses entries: proves nothing */
	rc = bt_cursor_open(bt, key, (uint16_t)klen, &cur);
	if (rc == XTC_OK && (xs_uniq_fail_now() || xs_rd_fault())) {
		bt_cursor_close(cur);
		rc = XTC_E_IO;
	}
	if (rc != XTC_OK) /* planted 72: the error reads as "no duplicate" */
		return XTC_DST_BUG(XTC_DST_BUG_UNIQPROBE) ? 0 : xs_rd_count(rc);
	for (;;) {
		const void *k = NULL, *vv = NULL; uint16_t kl = 0, vl = 0;
		int64_t rid;
		int rl, kl2;
		if ((rc = bt_cursor_next(cur, &k, &kl, &vv, &vl)) != XTC_OK) {
			if (rc != XTC_E_NOTFOUND) /* else: the end of the tree */
				dup = rc;
			break;
		}
		if (kl != klen + 8 || memcmp(k, key, (size_t)klen) != 0) break;
		rid = (int64_t)(xs_get_be64((const uint8_t *)k + klen) ^
		    0x8000000000000000ull);
		if (rid == rowid) continue;
		if (cx != NULL && cx->in_txn && wbuf_find(cx, rt->tableid, rid) != NULL)
			continue;
		bt_cursor_park(cur);
		if (rec == NULL && (rec = malloc(XS_VMAX)) == NULL) {
			dup = XTC_E_NOMEM;
			break;
		}
		if (xs_rd_fault())
			rl = XTC_E_IO;
		else
			rl = xs_read_rec(bt, rt->tableid, rid, rec, XS_VMAX);
		if (rl < 0 && XTC_DST_BUG(XTC_DST_BUG_UNIQREAD))
			rl = 0; /* planted 80: the unread candidate is absent */
		if (rl < 0) { /* the candidate is unknown, not absent */
			dup = rl;
			break;
		}
		if (rl > 0) {
			kl2 = xs_idx_rowkey(rt, rec, rl, rid, 0, kk, (int)sizeof kk, NULL);
			if (xs_uniq_same(rt, kk, kl2, key, klen, rec, rl, rid,
			        w)) {
				dup = 1;
				break;
			}
		}
		rc = bt_cursor_resume(cur);
		if (rc == XTC_OK && xs_rd_fault())
			rc = XTC_E_IO;
		if (rc != XTC_OK) {
			dup = rc;
			break;
		}
	}
	bt_cursor_close(cur);
	free(rec);
	if (dup < 0 && dup != XTC_E_NOMEM)
		(void)xs_rd_count(dup);
	return dup;
}

/* xs_uniq_buffered_dup over the whole buffer, for when the umap misses
 * a row: every newest live buffered row of the index's table.  Same
 * answers. */
static int
xs_uniq_buffered_walk(xstore_ctx_t *c, const struct xs_idx_rt *rt,
    const uint8_t *key, int klen, const struct xs_urow *w)
{
	uint8_t kk[XS_IDX_KMAX];
	const uint8_t *rec;
	int wi, rl, kl2, rc;
	for (wi = 0; wi < c->wn; wi++) {
		const xs_wrec_t *b = &c->wbuf[wi];
		if (b->tableid != rt->tableid || b->rowid == w->rowid ||
		    b->deleted || !wbuf_is_newest(c, wi))
			continue;
		rc = xs_wrec_payload(c, wi, &rec, &rl);
		if (rc != XTC_OK)
			return rc == XTC_E_NOMEM ? rc : xs_rd_count(rc);
		kl2 = xs_idx_rowkey(rt, rec, rl, b->rowid, 0, kk,
		    (int)sizeof kk, NULL);
		if (xs_uniq_same(rt, kk, kl2, key, klen, rec, rl,
		        b->rowid, w))
			return 1;
	}
	return 0;
}

/* Does a buffered row of this transaction other than `rowid` carry key
 * tuple `key` in index `rt`?  1, 0, or a negative XTC_E_* code if a
 * buffered row could not be read. */
static int
xs_uniq_buffered_dup(xstore_ctx_t *c, const struct xs_idx_rt *rt,
    const uint8_t *key, int klen, const struct xs_urow *w)
{
	int64_t rowid = w->rowid;
	uint8_t kk[XS_IDX_KMAX];
	uint64_t h;
	int i, m, wi, rl, kl2, rc;
	const uint8_t *rec;
	if (!c->in_txn) return 0;
	if (c->umap_bad)
		return xs_uniq_buffered_walk(c, rt, key, klen, w);
	if (c->umap_cap == 0) return 0;
	h = xs_bytes_hash(key, klen);
	m = c->umap_cap - 1;
	for (i = (int)(h & (uint64_t)m); c->umap[i].wi >= 0; i = (i + 1) & m) {
		if (c->umap[i].h != h || c->umap[i].idx_id != rt->idx_id) continue;
		wi = c->umap[i].wi;
		if (wi >= c->wn || c->wbuf[wi].rowid == rowid) continue;
		if (!wbuf_is_newest(c, wi) || c->wbuf[wi].deleted)
			continue; /* superseded or deleted in this txn */
		rc = xs_wrec_payload(c, wi, &rec, &rl);
		if (rc != XTC_OK) /* a spilled payload not read back */
			return rc == XTC_E_NOMEM ? rc : xs_rd_count(rc);
		kl2 = xs_idx_rowkey(rt, rec, rl, c->wbuf[wi].rowid, 0, kk,
		    (int)sizeof kk, NULL);
		if (xs_uniq_same(rt, kk, kl2, key, klen, rec, rl,
		        c->wbuf[wi].rowid, w))
			return 1;
	}
	return 0;
}

/*
 * Statement-time UNIQUE check for writing `rec` at `rowid` of `tableid`
 * on connection `cx`: every unique index's key tuple must not be carried
 * by another live committed row or another of this transaction's
 * buffered rows.  On a violation formats the SQLite message into
 * cx->uniq_err and returns 1; if the committed probe fails, formats an
 * "index error" message there and returns -1; else 0.  The commit
 * re-validates against commits this check could not see.
 */
static int
xs_uniq_check_stmt(xstore_ctx_t *cx, uint32_t tableid, int64_t rowid,
    const uint8_t *rec, int reclen)
{
	struct xs_idx_rt hits[16];
	struct xs_urow w = { rec, reclen, rowid };
	uint8_t key[XS_IDX_KMAX];
	uint64_t vis;
	int nh, i, klen, isnull, d;
	if (!XS_IDX_ANY() || rec == NULL) return 0;
	nh = xs_idx_for_table(cx->bt, tableid, hits, 16);
	if (nh == 0) return 0;
	vis = xs_visible();
	for (i = 0; i < nh; i++) {
		struct xs_idx_ent e;
		if (!hits[i].unique) continue;
		klen = xs_idx_rowkey(&hits[i], rec, reclen, rowid, 0, key,
		    (int)sizeof key, &isnull);
		if (klen < 0 || isnull) continue;
		d = xs_uniq_buffered_dup(cx, &hits[i], key, klen, &w);
		if (d == 0)
			d = xs_uniq_committed_dup(cx->bt, &hits[i], key, klen,
			    &w, cx);
		if (d != 0) {
			if (d < 0)
				xs_idx_err_msg(cx->bt, hits[i].idx_id, d,
				    cx->uniq_err, sizeof cx->uniq_err);
			else if (xs_idx_get(cx->bt, hits[i].idx_id, &e))
				xs_uniq_msg(&e, cx->uniq_err, sizeof cx->uniq_err);
			return d < 0 ? -1 : 1;
		}
	}
	if (cx->in_txn && (cx->uniq_vis == 0 || vis < cx->uniq_vis)) {
		cx->uniq_vis = vis;
		cx->uniq_gen = xtc_atomic_load_u64(&g_idx_gen, xtc_mo_acquire);
	}
	return 0;
}

/* Does an in-flight commit write a live row other than `rowid` whose
 * key tuple in `rt` is `key`?  Caller holds the commit lock. */
static int
xs_uniq_inflight_dup(const struct xs_idx_rt *rt, const uint8_t *key, int klen,
    const struct xs_urow *w, const xstore_ctx_t *self)
{
	int64_t rowid = w->rowid;
	const struct xs_inflight *e;
	uint8_t kk[XS_IDX_KMAX];
	int i, rl, kl2;
	for (e = g_inflight; e != NULL; e = e->next) {
		if (e->cx == self && self != NULL) continue;
		if (e->ws == NULL) {
			if (e->tid != rt->tableid || e->rid == rowid || e->rec == NULL)
				continue;
			kl2 = xs_idx_rowkey(rt, e->rec, e->reclen, e->rid, 0, kk,
			    (int)sizeof kk, NULL);
			if (xs_uniq_same(rt, kk, kl2, key, klen, e->rec,
			        e->reclen, e->rid, w))
				return 1;
			continue;
		}
		for (i = 0; i < e->wn; i++) {
			const uint8_t *rec;
			if (e->ws[i].tableid != rt->tableid || e->ws[i].rowid == rowid)
				continue;
			if (e->ws[i].deleted || e->ws[i].data == NULL ||
			    (e->cx != NULL && !wbuf_is_newest(e->cx, i)))
				continue;          /* deleted or superseded */
			rec = e->ws[i].data; rl = (int)e->ws[i].len;
			kl2 = xs_idx_rowkey(rt, rec, rl, e->ws[i].rowid, 0, kk,
			    (int)sizeof kk, NULL);
			if (xs_uniq_same(rt, kk, kl2, key, klen, rec, rl,
			        e->ws[i].rowid, w))
				return 1;
		}
	}
	return 0;
}

/* The unique indexes on the tables a write set touches (copied before
 * the commit lock is taken: the index cache sits behind the catalog
 * lock, which is taken before -- never under -- the commit lock).
 * Returns the count. */
static int
xs_uniq_collect(bt_t *bt, const xstore_ctx_t *cx, uint32_t tid1,
    struct xs_idx_rt *u, int cap)
{
	struct xs_idx_rt hits[16];
	uint32_t seen[16];
	int nseen = 0, n = 0, wi, nw, i, j, nh;
	if (!XS_IDX_ANY()) return 0;
	nw = cx != NULL ? cx->wn : 1;
	for (wi = 0; wi < nw; wi++) {
		uint32_t tid = cx != NULL ? cx->wbuf[wi].tableid : tid1;
		for (j = 0; j < nseen; j++) if (seen[j] == tid) break;
		if (j < nseen) continue;
		if (nseen < 16) seen[nseen++] = tid;
		nh = xs_idx_for_table(bt, tid, hits, 16);
		for (i = 0; i < nh && n < cap; i++)
			if (hits[i].unique) u[n++] = hits[i];
	}
	return n;
}

/*
 * Commit-time UNIQUE validation of a write set (caller holds the commit
 * lock, so the committed tree plus the in-flight registry is exactly
 * the state every earlier committer left).  `cx` is the committing
 * transaction, or NULL for one autocommit write (tid1/rid1/rec1).
 * `u` are its tables' unique indexes (xs_uniq_collect).  A table no
 * commit has touched since the transaction's statement-time checks
 * (g_tab_cts <= uniq_vis, with no index created since) is skipped:
 * everything up to that horizon was applied when the checks ran.
 * Returns 1 with *bad set to the violated index on a conflict, or the
 * probe's negative XTC_E_* code with *bad set to its index when the
 * committed probe fails (the commit must fail: nothing is proven).
 */
static int
xs_uniq_validate_locked(bt_t *bt, xstore_ctx_t *cx, uint32_t tid1,
    int64_t rid1, const uint8_t *rec1, int len1,
    const struct xs_idx_rt *u, int nu, uint32_t *bad)
{
	struct xs_urow w;
	uint8_t key[XS_IDX_KMAX];
	int n = cx != NULL ? cx->wn : 1, wi, i, klen, isnull, rl = 0;
	int skipok, d;
	if (nu == 0) return 0;
	skipok = cx != NULL && cx->uniq_vis != 0 &&
	    cx->uniq_gen == xtc_atomic_load_u64(&g_idx_gen, xtc_mo_acquire);
	for (wi = 0; wi < n; wi++) {
		uint32_t tid = cx != NULL ? cx->wbuf[wi].tableid : tid1;
		int64_t rid = cx != NULL ? cx->wbuf[wi].rowid : rid1;
		const uint8_t *rec;
		if (cx != NULL) {
			if (cx->wbuf[wi].deleted || !wbuf_is_newest(cx, wi))
				continue;
			if (skipok && g_tab_cts[tid % XS_TAB_TS_N] <= cx->uniq_vis)
				continue;
			rec = cx->wbuf[wi].data; rl = (int)cx->wbuf[wi].len;
		} else {
			rec = rec1; rl = len1;
		}
		if (rec == NULL) continue;
		for (i = 0; i < nu; i++) {
			if (u[i].tableid != tid) continue;
			klen = xs_idx_rowkey(&u[i], rec, rl, rid, 0, key,
			    (int)sizeof key, &isnull);
			if (klen < 0 || isnull) continue;
			/* A key no commit carried since the statement check
			 * (skipok also means no index was created since). */
			if (skipok && g_uk_cts[xs_uk_slot(u[i].idx_id, key, klen)] <=
			    cx->uniq_vis)
				continue;
			w.rec = rec;
			w.reclen = rl;
			w.rowid = rid;
			d = xs_uniq_committed_dup(bt, &u[i], key, klen, &w, cx);
			if (d == 0)
				d = xs_uniq_inflight_dup(&u[i], key, klen, &w,
				    cx);
			if (d != 0) {
				*bad = u[i].idx_id;
				return d;
			}
		}
	}
	return 0;
}

/* Commit lock held, `ts` just minted: stamp every UNIQUE key tuple the
 * write set (cx) or the one autocommit record (tid1/rec1) carries. */
static void
xs_uniq_stamp_locked(const xstore_ctx_t *cx, uint32_t tid1,
    const uint8_t *rec1, int len1, int64_t rid1,
    const struct xs_idx_rt *u, int nu, uint64_t ts)
{
	uint8_t key[XS_IDX_KMAX];
	int n = cx != NULL ? cx->wn : 1, wi, i, klen, isnull, rl;
	for (wi = 0; wi < n && nu > 0; wi++) {
		uint32_t tid = cx != NULL ? cx->wbuf[wi].tableid : tid1;
		int64_t rid = cx != NULL ? cx->wbuf[wi].rowid : rid1;
		const uint8_t *rec = cx != NULL ? cx->wbuf[wi].data : rec1;
		if (cx != NULL && cx->wbuf[wi].deleted) continue;
		rl = cx != NULL ? (int)cx->wbuf[wi].len : len1;
		if (rec == NULL) continue;
		for (i = 0; i < nu; i++) {
			if (u[i].tableid != tid) continue;
			klen = xs_idx_rowkey(&u[i], rec, rl, rid, 0, key,
			    (int)sizeof key, &isnull);
			if (klen >= 0 && !isnull)
				g_uk_cts[xs_uk_slot(u[i].idx_id, key, klen)] = ts;
		}
	}
}

/* ---- verified index range scan ---------------------------------- */

struct xs_icand {
	uint8_t *key;     /* full entry key (tuple + rowid) */
	int      klen;
	int64_t  rowid;
};

/* A group of truncated entries that share their first P tuple bytes,
 * read whole and re-sorted by whole key tuple, then rowid, so the scan's
 * order is exact (xs_iscan_group).  Each row's buf holds its record,
 * then its whole tuple. */
struct xs_igrow {
	int64_t rid;
	int rl, tl;
	uint8_t *buf;
};
struct xs_igrp {
	struct xs_igrow *row;
	int n, i, cap;
	int have_next; /* the row read past the group, held: */
	int64_t next_rid;
	int next_rl, next_kl;
	uint8_t next_rec[XS_VMAX], next_k[XS_IDX_KMAX];
};

/* A scan's bounds: the equality prefix, then an optional range on the
 * next column (see xs_bnd_where). */
struct xs_ibnd {
	const uint8_t *pfx, *lo, *hi;
	int pfxlen, lolen, hilen;
	int lo_incl, hi_incl, has_lo, has_hi, range;
};

/* The whole (uncut) bounds of a scan whose bound is longer than an
 * entry holds. */
struct xs_ifull {
	uint8_t pfx[XS_IDX_TMAX], lo[XS_IDX_TMAX], hi[XS_IDX_TMAX];
	struct xs_ibnd b;
};

/*
 * An index scan.  The descriptor is small: the key buffers (pfx, lo, hi,
 * pos, ck) hold one entry key each, rt.kmax = bt_max_key() bytes, and
 * live in the same allocation after it (keys[]); the row buffer (rec)
 * and the read-ahead batches are allocated when first needed.  Only the
 * descriptor itself is zeroed at open.
 */
struct xstore_iscan {
	struct xsql  *db;
	xstore_ctx_t *cx;
	bt_t         *bt;
	struct xs_idx_rt rt;
	uint64_t      snap;
	int           reverse;
	int           done;
	int           err;               /* first read error (XTC_E_*), sticky */
	/* bounds, in entry space (a long bound cut to its group) */
	uint8_t      *pfx;  int pfxlen;  /* ns + equality elems */
	uint8_t      *lo;   int lolen, lo_incl, has_lo;
	uint8_t      *hi;   int hilen, hi_incl, has_hi;
	int           range;             /* a range on the next column */
	struct xs_ibnd eb;               /* views of the bounds above */
	struct xs_ifull *fb;             /* whole bounds, if a bound was cut */
	struct xs_igrp *grp;             /* a re-sorted group (xs_iscan_group) */
	const uint8_t *lk;               /* entry key of the row just taken */
	int           lkl;
	/* committed stream */
	bt_cursor_t  *cur;
	/* started: forward, the equality probe was tried (xs_iscan_point,
	 * once per scan); reverse, the first batch was read.  pos: the
	 * walk's position (forward, the probe's hand-off key; reverse, the
	 * last key read). */
	uint8_t      *pos;  int poslen, started;
	uint8_t      *ck;   int cklen, have_ck; int64_t crid;
	/* Read-ahead of index keys (xs_iscan_fill): in-range entries copied
	 * from one cursor resume (forward) or one descent (reverse) instead
	 * of a resume or a whole bt_seek_lt descent per entry.  kb_end: the
	 * walk has passed the range's end, nothing is left after the batch. */
	struct xs_ika *ka;
	int           kb_n, kb_i, kb_want, kb_end;
	/* this transaction's buffered rows in range, sorted */
	struct xs_icand *bc;
	int           nbc, bci;
	uint8_t      *bkeys;
	/* output: the stored value, flag byte first (xs_point_get), 1 +
	 * XS_VMAX bytes once a row needs it (xs_iscan_rec) */
	uint8_t      *rec;
	uint64_t      visited;
	/* Row read-ahead (xs_iscan_rows): the visible versions of the next
	 * committed candidates, fetched by one batched seek (bt_seek_ge_n)
	 * instead of a root-to-leaf seek per row.  A hint (xstore_iscan_hint:
	 * LIMIT, MIN/MAX) sizes the first batches so a short walk does not
	 * read rows it never returns. */
	struct xs_ira *ra;
	int           ra_n, ra_i, ra_end;
	int           ra_left;   /* hinted rows not yet read (0: no hint) */
	uint8_t       keys[];    /* the key buffers: XS_ISCAN_NKEY x rt.kmax */
};
#define XS_ISCAN_NKEY 5

/* The scan's row buffer (1 + XS_VMAX bytes), or NULL with s->err set. */
static uint8_t *
xs_iscan_rec(xstore_iscan_t *s)
{
	if (s->rec == NULL && (s->rec = malloc(1 + XS_VMAX)) == NULL)
		s->err = XTC_E_NOMEM;
	return s->rec;
}

/* Compare key k[0..kl) with bound b[0..bl) as a prefix: 0 if k starts
 * with b, else the sign of their first difference (a k that ends first
 * sorts before). */
static int
xs_pfxcmp(const uint8_t *k, int kl, const uint8_t *b, int bl)
{
	int r = memcmp(k, b, (size_t)(kl < bl ? kl : bl));
	if (r == 0 && kl < bl)
		return -1;
	return r;
}

/* Is key `k` (length kl) inside bounds `b`?  Returns 1 in range, 0
 * before the start (forward: skip; reverse: stop), 2 past the end
 * (forward: stop; reverse: skip).  Used on entry keys with the entry
 * bounds, and on a row's whole key tuple with the whole bounds. */
static int
xs_bnd_where(const struct xs_ibnd *b, const uint8_t *k, int kl)
{
	int r;
	r = xs_pfxcmp(k, kl, b->pfx, b->pfxlen);
	if (r != 0)
		return r < 0 ? 0 : 2;
	if (b->has_lo) {
		r = xs_pfxcmp(k, kl, b->lo, b->lolen);
		if (r < 0 || (r == 0 && !b->lo_incl))
			return 0;
	} else if (b->range && kl > b->pfxlen && k[b->pfxlen] <= XS_TUP_NULL) {
		return 0;                         /* NULL never satisfies a range */
	}
	if (b->has_hi) {
		r = xs_pfxcmp(k, kl, b->hi, b->hilen);
		if (r > 0 || (r == 0 && !b->hi_incl))
			return 2;
	}
	return 1;
}

/* Is entry key `k` (length kl) inside the scan's entry bounds? */
static int
xs_iscan_where(const struct xstore_iscan *s, const uint8_t *k, int kl)
{
	return xs_bnd_where(&s->eb, k, kl);
}

/* Does a candidate row's record rec[0..rl) satisfy the scan's whole
 * bounds?  Always, unless a bound was cut to fit an entry: then the
 * entries in range are a superset and the row's whole key decides. */
static int
xs_iscan_row_ok(const struct xstore_iscan *s, const uint8_t *rec, int rl,
    int64_t rowid)
{
	static _Thread_local uint8_t tup[XS_IDX_TMAX];
	int tl;
	if (s->fb == NULL)
		return 1;
	tl = xs_idx_tuple(&s->rt, rec, rl, rowid, tup, (int)sizeof tup, NULL);
	return tl >= 0 && xs_bnd_where(&s->fb->b, tup, tl) == 1;
}

static int
xs_icand_cmp(const void *pa, const void *pb)
{
	const struct xs_icand *a = pa, *b = pb;
	int r = memcmp(a->key, b->key, (size_t)(a->klen < b->klen ? a->klen : b->klen));
	if (r) return r;
	return a->klen - b->klen;
}

/* Effective read snapshot for connection ctx (see xstore_scan_open_txn). */
static uint64_t
xs_eff_snap(const xstore_ctx_t *ctx)
{
	uint64_t rs;
	if (ctx == NULL) return xs_visible();
	rs = xtc_atomic_load_u64(&ctx->read_snap, xtc_mo_relaxed);
	if (rs != 0) return rs;
	if (ctx->in_txn && ctx->txn_iso >= XS_ISO_REPEATABLE_READ) return ctx->txn_snap;
	if (ctx->stmt_base != 0) return ctx->stmt_base;
	return xs_visible();
}

/* The version of (tid, rid) visible at `snap`: copies its whole stored
 * value (the flag byte, then the payload) into buf (cap >= 1 + the
 * payload) and returns the payload length (>= 1; -1 for a live empty
 * record), 0 if no version is visible.  A tree that could not be read
 * also returns 0, with the XTC_E_* code in *err if err is not NULL.
 * Ignores the write buffer (the caller handles buffered rows).  One
 * probe: version keys sort newest first within a rowid (inverted
 * commit_ts), so the first key >= the key (tid, rid, snap) is the
 * newest version with commit_ts <= snap -- no walk over newer versions,
 * no cursor, and no latch on the OLC path (bt_seek_ge). */
static int
xs_point_get(bt_t *bt, uint32_t tid, int64_t rid, uint64_t snap, uint8_t *buf,
    int cap, int *err)
{
	/* The first key >= the probe can belong to the next table or to the
	 * index namespace, so k holds the longest key there is. */
	uint8_t probe[XS_VKLEN], k[XS_IDX_KMAX];
	uint16_t kl = 0, vl = 0;
	int rc;
	enc_vkey(tid, rid, snap, probe);
	rc = bt_seek_ge(bt, probe, XS_VKLEN, k, (uint16_t)sizeof k, &kl, buf,
	    (uint16_t)(cap > 0xFFFF ? 0xFFFF : cap), &vl);
	if (rc != XTC_OK && rc != XTC_E_NOTFOUND && err != NULL)
		*err = rc;
	if (rc != XTC_OK || kl != XS_VKLEN || dec_tableid(k) != tid ||
	    dec_rowid(k) != rid || vl < 1 || (buf[0] & XS_F_DELETED))
		return 0;
	if (vl > cap) vl = (uint16_t)cap;
	return vl > 1 ? (int)vl - 1 : -1;   /* -1: live, empty record */
}

/* Encode bounds `b` (prefix eq[0..neq), then lo / hi on the next
 * column) into the three buffers of `cap` bytes.  -1 if one overflows. */
static int
xs_bnd_encode(struct xs_ibnd *b, uint8_t *pfx, uint8_t *lob, uint8_t *hib,
    int cap, uint32_t idx_id, uint32_t descmask, const xstore_val_t *eq,
    int neq, const xstore_val_t *lo, const xstore_val_t *hi)
{
	int i, off;
	off = xs_idx_prefix(idx_id, pfx);
	for (i = 0; i < neq && off >= 0; i++)
		off = xs_tup_elem(pfx, off, cap, eq[i].cls, eq[i].i, eq[i].r,
		    eq[i].p, eq[i].n, (int)((descmask >> i) & 1u));
	if (off < 0)
		return -1;
	b->pfx = pfx;
	b->pfxlen = off;
	b->lo = lob;
	b->hi = hib;
	b->range = (lo != NULL || hi != NULL);
	if (lo != NULL) {
		memcpy(lob, pfx, (size_t)off);
		b->lolen = xs_tup_elem(lob, off, cap, lo->cls, lo->i, lo->r,
		    lo->p, lo->n, 0);
		if (b->lolen < 0)
			return -1;
	}
	if (hi != NULL) {
		memcpy(hib, pfx, (size_t)off);
		b->hilen = xs_tup_elem(hib, off, cap, hi->cls, hi->i, hi->r,
		    hi->p, hi->n, 0);
		if (b->hilen < 0)
			return -1;
	}
	return 0;
}

/* Cut a whole bound w[0..wl) (prefix included) to the entries it can
 * match, into out: itself if its tuple part fits an entry whole (at
 * most P = kmax - 21 bytes), else its P-byte group, plus the hash when
 * `full` (an equality on every column: w is a whole key tuple; a range
 * bound never gets it, hashes do not sort as values).  Returns the cut
 * length. */
static int
xs_bnd_cut(const struct xs_idx_rt *rt, const uint8_t *w, int wl, int full,
    uint8_t *out)
{
	int p = rt->kmax - 21;
	if (wl - 5 <= p) {
		memcpy(out, w, (size_t)wl);
		return wl;
	}
	if (full)
		return xs_idx_entkey(rt, w, wl, 0, 0, out, rt->kmax);
	memcpy(out, w, (size_t)(5 + p));
	return 5 + p;
}

/* Point the scan's entry-bound view (s->eb) at its bound buffers.  A
 * cut scan leaves "NULL never satisfies a range" to the whole bounds:
 * in a cut entry the byte after the prefix is not an element tag. */
static void
xs_iscan_view(xstore_iscan_t *s)
{
	s->eb.pfx = s->pfx;
	s->eb.pfxlen = s->pfxlen;
	s->eb.lo = s->lo;
	s->eb.lolen = s->lolen;
	s->eb.hi = s->hi;
	s->eb.hilen = s->hilen;
	s->eb.lo_incl = s->lo_incl;
	s->eb.hi_incl = s->hi_incl;
	s->eb.has_lo = s->has_lo;
	s->eb.has_hi = s->has_hi;
	s->eb.range = s->fb == NULL ? s->range : 0;
}

/*
 * Set the scan's bounds.  The common case encodes them straight into
 * the scan's entry bounds, capped at the longest tuple part an entry
 * holds whole.  A bound longer than that is encoded whole into s->fb
 * and cut to its group (xs_bnd_cut): the cut bounds then match a
 * superset of the entries, a cut low or high bound turns inclusive, and
 * each candidate row is checked against the whole bounds
 * (xs_iscan_row_ok).  Returns 0, or -1 (the caller scans instead).
 */
static int
xs_iscan_bounds(xstore_iscan_t *s, const xstore_val_t *eq, int neq,
    const xstore_val_t *lo, int lo_incl, const xstore_val_t *hi, int hi_incl)
{
	const struct xs_idx_rt *rt = &s->rt;
	struct xs_ifull *fb;
	int p = rt->kmax - 21;
	if (p < 1)
		return -1;
	s->range = lo != NULL || hi != NULL;
	s->has_lo = lo != NULL;
	s->lo_incl = lo_incl;
	s->has_hi = hi != NULL;
	s->hi_incl = hi_incl;
	if (xs_bnd_encode(&s->eb, s->pfx, s->lo, s->hi, 5 + p, rt->idx_id,
	        rt->descmask, eq, neq, lo, hi) == 0) {
		s->pfxlen = s->eb.pfxlen;
		s->lolen = s->eb.lolen;
		s->hilen = s->eb.hilen;
		xs_iscan_view(s);
		return 0;
	}
	if ((fb = malloc(sizeof *fb)) == NULL)
		return -1;
	memset(&fb->b, 0, sizeof fb->b);
	if (xs_bnd_encode(&fb->b, fb->pfx, fb->lo, fb->hi, XS_IDX_TMAX,
	        rt->idx_id, rt->descmask, eq, neq, lo, hi) != 0) {
		free(fb);
		return -1;
	}
	fb->b.has_lo = lo != NULL;
	fb->b.lo_incl = lo_incl;
	fb->b.has_hi = hi != NULL;
	fb->b.hi_incl = hi_incl;
	s->fb = fb;
	s->pfxlen =
	    xs_bnd_cut(rt, fb->pfx, fb->b.pfxlen, neq == rt->ncol, s->pfx);
	if (lo != NULL) {
		s->lolen = xs_bnd_cut(rt, fb->lo, fb->b.lolen, 0, s->lo);
		if (s->lolen < fb->b.lolen)
			s->lo_incl = 1;
	}
	if (hi != NULL) {
		s->hilen = xs_bnd_cut(rt, fb->hi, fb->b.hilen, 0, s->hi);
		if (s->hilen < fb->b.hilen)
			s->hi_incl = 1;
	}
	xs_iscan_view(s);
	return 0;
}

xstore_iscan_t *
xstore_iscan_open(struct xsql *db, const char *table, uint32_t idx_id,
    const xstore_val_t *eq, int neq, const xstore_val_t *lo, int lo_incl,
    const xstore_val_t *hi, int hi_incl, int reverse)
{
	bt_t *bt = xstore_bt_of(db);
	xstore_iscan_t *s;
	struct xs_idx_rt rt;
	int i;
	(void)table;
	if (bt == NULL) return NULL;
	if (!xs_idx_get_rt(bt, idx_id, &rt) || rt.tableid == 0 ||
	    rt.broken != 0 || neq > rt.ncol || ((lo || hi) && neq >= rt.ncol) ||
	    ((lo || hi) && ((rt.descmask >> neq) & 1u)))
		return NULL;
	s = malloc(sizeof *s + (size_t)XS_ISCAN_NKEY * (size_t)rt.kmax);
	if (s == NULL)
		return NULL;
	memset(s, 0, sizeof *s);
	s->rt = rt;
	s->pfx = s->keys;
	s->lo = s->pfx + rt.kmax;
	s->hi = s->lo + rt.kmax;
	s->pos = s->hi + rt.kmax;
	s->ck = s->pos + rt.kmax;
	s->db = db;
	s->bt = bt;
	s->cx = xstore_ctx_of(db);
	if (s->cx != NULL) xs_enter_ctx(s->cx, db);
	s->snap = xs_eff_snap(s->cx);
	s->reverse = reverse;
	if (xs_iscan_bounds(s, eq, neq, lo, lo_incl, hi, hi_incl) != 0) {
		xstore_iscan_close(s);
		return NULL;
	}
	xtc_atomic_fetch_add_u64(&g_idx_seeks, 1, xtc_mo_relaxed);
	if (s->cx != NULL && s->cx->in_txn) {
		s->cx->touched = 1;
		if (s->cx->serializable)
			s->cx->did_scan = 1;   /* a range read: conservative SSI edge */
	}

	/* This transaction's buffered rows of the table whose key falls in
	 * range: merged with the committed stream in key order.  Committed
	 * candidates with a buffered write are skipped there. */
	if (s->cx != NULL && s->cx->in_txn && s->cx->wn > 0) {
		xstore_ctx_t *cx = s->cx;
		uint8_t key[XS_IDX_KMAX];
		size_t boff = 0;
		int wi, rl, kl, rc;
		for (wi = 0; wi < cx->wn; wi++) {
			const uint8_t *rec;
			if (cx->wbuf[wi].tableid != s->rt.tableid ||
			    cx->wbuf[wi].deleted || !wbuf_is_newest(cx, wi))
				continue;
			rc = xs_wrec_payload(cx, wi, &rec, &rl);
			if (rc != XTC_OK) { /* the first next fails */
				s->err = rc == XTC_E_NOMEM ? rc : xs_rd_count(rc);
				break;
			}
			kl = xs_idx_rowkey(&s->rt, rec, rl, cx->wbuf[wi].rowid,
			    1, key, (int)sizeof key, NULL);
			if (kl < 0 || xs_iscan_where(s, key, kl) != 1 ||
			    !xs_iscan_row_ok(s, rec, rl, cx->wbuf[wi].rowid))
				continue;
			if (s->nbc % 64 == 0) {
				struct xs_icand *nb = realloc(s->bc, sizeof *nb * (size_t)(s->nbc + 64));
				uint8_t *nk;
				if (nb != NULL)
					s->bc = nb;
				nk = realloc(s->bkeys,
				    boff + (size_t)64 * (size_t)s->rt.kmax);
				if (nk != NULL)
					s->bkeys = nk;
				if (nb == NULL || nk == NULL) {
					xstore_iscan_close(s);
					return NULL;
				}
			}
			memcpy(s->bkeys + boff, key, (size_t)kl);
			s->bc[s->nbc].key = (uint8_t *)(uintptr_t)boff;
			s->bc[s->nbc].klen = kl;
			s->bc[s->nbc].rowid = cx->wbuf[wi].rowid;
			boff += (size_t)kl;
			s->nbc++;
		}
		for (i = 0; i < s->nbc; i++)
			s->bc[i].key = s->bkeys + (size_t)(uintptr_t)s->bc[i].key;
		if (s->nbc > 1) {
			qsort(s->bc, (size_t)s->nbc, sizeof *s->bc, xs_icand_cmp);
			if (reverse)
				for (i = 0; i < s->nbc / 2; i++) {
					struct xs_icand t = s->bc[i];
					s->bc[i] = s->bc[s->nbc - 1 - i];
					s->bc[s->nbc - 1 - i] = t;
				}
		}
	}
	return s;
}

/* A batch of index keys: up to XS_IKA_MAX, packed in buf (a batch stops
 * once XS_IKA_BYTES are used, so one more maximal key always fits). */
#define XS_IKA_MAX   128
#define XS_IKA_BYTES (8 * 1024)
struct xs_ika {
	uint32_t off[XS_IKA_MAX];
	uint16_t len[XS_IKA_MAX];
	uint8_t  buf[];     /* XS_IKA_BYTES + rt.kmax */
};

/* Record the scan's first error (sticky: every later call fails too)
 * and return -1.  The cursor, if any, is parked: a failed scan holds no
 * latch until it is closed. */
static int
xs_iscan_fail(xstore_iscan_t *s, int rc)
{
	if (s->err == 0)
		s->err = rc == XTC_E_NOMEM ? rc : xs_rd_count(rc);
	if (s->cur != NULL)
		(void)bt_cursor_park(s->cur);
	return -1;
}

/*
 * The equality probe: the first fill of a forward scan whose bounds are
 * an equality prefix alone (no range on a next column).  Its entries
 * share the prefix and are almost always in one leaf, so they are read
 * with one descent and no cursor (bt_seek_ge_keys).  If the read holds
 * every entry with the prefix, they are the whole range: the batch is
 * final (kb_end) and the walk never opens a cursor.  Otherwise (more
 * entries than one batch, or the prefix runs on into the next leaf) the
 * batch stands and the cursor walk continues after its last key
 * (s->pos).  Returns the count (0 with kb_end: no entry; 0 without it:
 * no entry in this leaf, the cursor walk starts at the bound), or -1 on
 * an error (s->err).
 *
 * A HASH index (STORAGE_DESIGN section 10) fills the same key batch
 * from its bucket for an equality, so the row read-ahead, the
 * verification against the row and the buffered-row merge after this
 * serve both index types.
 */
static int
xs_iscan_point(xstore_iscan_t *s)
{
	int got = 0, more = 0, rc;

	/* The whole batch, not the hinted size: the keys come from a leaf
	 * already latched, and the row read-ahead keeps to the hint. */
	rc = bt_seek_ge_keys(s->bt, s->pfx, (uint16_t)s->pfxlen,
	    (uint16_t)s->pfxlen, s->ka->buf,
	    XS_IKA_BYTES + (uint32_t)s->rt.kmax, s->ka->off, s->ka->len,
	    XS_IKA_MAX, &got, &more);
	if (rc == XTC_OK && xs_rd_fault())
		rc = XTC_E_IO;
	if (rc != XTC_OK)
		return xs_iscan_fail(s, rc);
	if (more && got > 0) {
		s->poslen = s->ka->len[got - 1];
		memcpy(s->pos, s->ka->buf + s->ka->off[got - 1],
		    (size_t)s->poslen);
	} else if (!more) {
		s->kb_end = 1;
	}
	return got;
}

/* DST pessimal point at the hand-off from the equality probe's batch to
 * the cursor walk: under the simulator with buggify on, stall the scan
 * 1 ms (virtual) on a seeded coin, so writes to the group (entries in
 * and out of it, splits and merges of its leaves) land between the batch
 * and the cursor's open.  A single relaxed load in production. */
static void
xs_iscan_yield(void)
{
	if (XTC_SIM_BUGGIFY("xstore.iscan.handoff") &&
	    xtc_sim_buggify_fault(500) && !xtc_pid_is_none(xtc_self()))
		(void)xtc_proc_sleep(1000LL * 1000);
}

/*
 * The forward cursor walk: open the scan's cursor (at the equality
 * probe's last key, or at the range's start) or resume it, and copy up
 * to kb_want in-range keys into the key batch.  The cursor is parked on
 * return, on success and on error alike, so no latch is held between
 * batches.  Returns the count, or -1 on an error (s->err).
 */
static int
xs_iscan_fwd(xstore_iscan_t *s)
{
	const void *k = NULL, *vv = NULL;
	uint16_t kl = 0, vl = 0;
	uint32_t used = 0;
	int w, n = 0, rc;

	/* Open or resume the cursor. */
	if (s->cur == NULL && s->poslen > 0) {
		/* After the equality probe's batch: its last key, if still
		 * there, is the cursor's first and is skipped. */
		xs_iscan_yield();
		rc = bt_cursor_open(s->bt, s->pos, (uint16_t)s->poslen,
		    &s->cur);
	} else if (s->cur == NULL) {
		const uint8_t *st = s->has_lo ? s->lo : s->pfx;
		int sl = s->has_lo ? s->lolen : s->pfxlen;
		rc = bt_cursor_open(s->bt, st, (uint16_t)sl, &s->cur);
	} else {
		rc = bt_cursor_resume(s->cur);
	}
	if (rc == XTC_OK && xs_rd_fault())
		rc = XTC_E_IO;
	if (rc != XTC_OK)
		return xs_iscan_fail(s, rc);
	/* Copy the in-range keys, skipping the hand-off key. */
	while (n < s->kb_want && used <= XS_IKA_BYTES) {
		rc = bt_cursor_next(s->cur, &k, &kl, &vv, &vl);
		if (rc == XTC_OK && xs_rd_fault())
			rc = XTC_E_IO;
		if (rc == XTC_E_NOTFOUND) {
			s->kb_end = 1; /* the end of the tree */
			break;
		}
		if (rc != XTC_OK)
			return xs_iscan_fail(s, rc);
		if (s->poslen > 0) {
			int same = kl == s->poslen &&
			    memcmp(k, s->pos, kl) == 0;
			s->poslen = 0;
			if (same)
				continue;
		}
		w = xs_iscan_where(s, k, kl);
		if (w == 2) { s->kb_end = 1; break; }
		if (w == 0) continue;
		memcpy(s->ka->buf + used, k, kl);
		s->ka->off[n] = used;
		s->ka->len[n] = kl;
		used += kl;
		n++;
	}
	bt_cursor_park(s->cur);
	return n;
}

/* Refill the key batch with the next in-range entries in walk order.
 * Returns the count (0: the range is exhausted), or -1 when the batch
 * cannot be allocated or the tree cannot be read (s->err). */
static int
xs_iscan_fill(xstore_iscan_t *s)
{
	int w, n = 0, rc;

	s->kb_n = s->kb_i = 0;
	if (s->err != 0)
		return -1;
	if (s->kb_end) return 0;
	if (s->ka == NULL) {
		s->ka = malloc(sizeof *s->ka + XS_IKA_BYTES + (size_t)s->rt.kmax);
		if (s->ka == NULL)
			return xs_iscan_fail(s, XTC_E_NOMEM);
	}
	/* First batch: the hinted row count (LIMIT 20 is one batch, not
	 * 16 then 32), else 16; then doubling. */
	s->kb_want = s->kb_want == 0 ? (s->ra_left > 0 ? s->ra_left : 16) :
	    s->kb_want * 2;
	if (s->kb_want > XS_IKA_MAX) s->kb_want = XS_IKA_MAX;
	if (!s->reverse && s->cur == NULL && !s->range && !s->started) {
		s->started = 1; /* tried once; the cursor walk follows */
		n = xs_iscan_point(s);
		if (n != 0 || s->kb_end) {
			s->kb_n = n < 0 ? 0 : n;
			return n;
		}
	}
	if (!s->reverse) {
		if ((n = xs_iscan_fwd(s)) < 0)
			return -1;
	} else {
		uint8_t bound[XS_IDX_KMAX + 16];
		int bl, got = 0, j;
		if (!s->started) {
			/* Below the first key past every key that starts with
			 * the bound (hi inclusive, or the prefix): its
			 * successor.  The lead byte 0xFE takes any carry. */
			if (s->has_hi) {
				memcpy(bound, s->hi, (size_t)s->hilen);
				bl = s->hilen;
			} else {
				memcpy(bound, s->pfx, (size_t)s->pfxlen);
				bl = s->pfxlen;
			}
			if (!s->has_hi || s->hi_incl) {
				while (bl > 1 && bound[bl - 1] == 0xFF)
					bl--;
				bound[bl - 1]++;
			}
			s->started = 1;
		} else {
			memcpy(bound, s->pos, (size_t)s->poslen);
			bl = s->poslen;
		}
		rc = bt_seek_lt_n(s->bt, bound, (uint16_t)bl, s->ka->buf,
		    XS_IKA_BYTES + (uint32_t)s->rt.kmax, s->ka->off, s->ka->len,
		    s->kb_want, &got);
		if (rc == XTC_OK && xs_rd_fault())
			rc = XTC_E_IO;
		if (rc != XTC_OK)
			return xs_iscan_fail(s, rc);
		if (got == 0) {
			s->kb_end = 1;
			return 0;
		}
		/* The next batch starts below the last key read. */
		memcpy(s->pos, s->ka->buf + s->ka->off[got - 1], s->ka->len[got - 1]);
		s->poslen = s->ka->len[got - 1];
		for (j = 0; j < got; j++) {
			w = xs_iscan_where(s, s->ka->buf + s->ka->off[j], s->ka->len[j]);
			if (w == 0) { s->kb_end = 1; break; }  /* below the start: done */
			if (w == 2) continue;                  /* above the end: skip */
			s->ka->off[n] = s->ka->off[j];
			s->ka->len[n] = s->ka->len[j];
			n++;
		}
	}
	s->kb_n = n;
	return n;
}

/* Advance the committed stream to its next in-range entry whose rowid
 * has no buffered write (s->ck / s->crid).  Returns 1, 0 at the end, or
 * -1 on an error (s->err). */
static int
xs_iscan_committed(xstore_iscan_t *s)
{
	for (;;) {
		if (s->kb_i >= s->kb_n) {
			int r = xs_iscan_fill(s);
			if (r <= 0) {
				if (r == 0 && !s->kb_end) continue;   /* all skipped */
				return r;
			}
		}
		s->cklen = s->ka->len[s->kb_i];
		memcpy(s->ck, s->ka->buf + s->ka->off[s->kb_i], (size_t)s->cklen);
		s->kb_i++;
		if (s->cklen < s->pfxlen + 8) continue;
		s->crid = (int64_t)(xs_get_be64(s->ck + s->cklen - 8) ^ 0x8000000000000000ull);
		if (s->cx != NULL && s->cx->in_txn &&
		    wbuf_find(s->cx, s->rt.tableid, s->crid) != NULL)
			continue;                     /* the buffered write decides */
		return 1;
	}
}

/* The row read-ahead batch: up to XS_IRA_MAX committed candidates (key
 * and rowid), each with the stored value (flag byte first) of its
 * version visible at the snapshot: rc 1 visible, 0 none, -1 not settled
 * by the batch (xs_point_get decides when the row is taken). */
#define XS_IRA_MAX    32
#define XS_IRA_KBYTES 2048   /* packed keys; one more maximal key fits */
struct xs_ira {
	int64_t  rid[XS_IRA_MAX];
	int      rc[XS_IRA_MAX];
	uint32_t voff[XS_IRA_MAX];
	uint16_t vlen[XS_IRA_MAX];
	uint16_t koff[XS_IRA_MAX], klen[XS_IRA_MAX];
	uint8_t  probe[XS_IRA_MAX][XS_VKLEN];
	uint8_t  vbuf[16 * 1024];    /* a row that does not fit: xs_point_get */
	uint8_t  kbuf[];             /* XS_IRA_KBYTES + rt.kmax */
};

/* Refill the row batch with the next committed candidates -- XS_IRA_MAX
 * at a time, or under a hint (xstore_iscan_hint) the rows it still
 * expects, then 2, 4, .. -- and read their visible versions with one
 * bt_seek_ge_n.  Returns the count (0: the committed stream is done),
 * or -1 on an error (s->err; rows already read are dropped with it). */
static int
xs_iscan_rows(xstore_iscan_t *s)
{
	struct xs_ira *a = s->ra;
	bt_probe_t pr[XS_IRA_MAX];
	uint32_t ku = 0;
	int i, n = 0, c = 0, want;

	if (a == NULL) {
		a = s->ra = malloc(sizeof *a + XS_IRA_KBYTES + (size_t)s->rt.kmax);
		if (a == NULL)
			return xs_iscan_fail(s, XTC_E_NOMEM);
	}
	/* A hinted walk reads exactly its budget first; past it (rows the
	 * WHERE or the snapshot dropped) it starts small again. */
	if (s->ra_left > 0) want = s->ra_left;
	else if (s->ra_left == 0) want = XS_IRA_MAX;
	else want = s->ra_n > 0 ? s->ra_n * 2 : 2;
	if (want > XS_IRA_MAX) want = XS_IRA_MAX;
	s->ra_n = s->ra_i = 0;
	while (n < want && ku <= XS_IRA_KBYTES &&
	    (c = xs_iscan_committed(s)) == 1) {
		memcpy(a->kbuf + ku, s->ck, (size_t)s->cklen);
		a->koff[n] = (uint16_t)ku;
		a->klen[n] = (uint16_t)s->cklen;
		ku += (uint32_t)s->cklen;
		a->rid[n] = s->crid;
		enc_vkey(s->rt.tableid, s->crid, s->snap, a->probe[n]);
		pr[n].key = a->probe[n];
		pr[n].klen = XS_VKLEN;
		n++;
	}
	if (c < 0)
		return -1;
	if (n == 0)
		return 0;
	if (s->ra_left > 0 && (s->ra_left -= n) <= 0)
		s->ra_left = -1;
	/* Every probe starts XTC_E_AGAIN; one the batch did not settle is
	 * re-read by xs_point_get, which returns its error. */
	(void)bt_seek_ge_n(s->bt, pr, n, a->vbuf, /* RDERR_OK: per probe */
	    (uint32_t)sizeof a->vbuf);
	for (i = 0; i < n; i++) {
		const uint8_t *k = a->vbuf + pr[i].koff, *v = a->vbuf + pr[i].voff;
		a->voff[i] = pr[i].voff;
		a->vlen[i] = pr[i].vlen;
		/* As xs_point_get: the first version key >= (tid, rid, snap) is
		 * the newest one committed at or before snap, if it is this
		 * row's and not a tombstone. */
		if (pr[i].rc == XTC_OK)
			a->rc[i] = pr[i].kolen == XS_VKLEN &&
			    dec_tableid(k) == s->rt.tableid &&
			    dec_rowid(k) == a->rid[i] && pr[i].vlen >= 1 &&
			    !(v[0] & XS_F_DELETED);
		else
			a->rc[i] = -1;
	}
	s->ra_n = n;
	return n;
}

/* Expect about `nrows` rows to be taken (a LIMIT, 1 for MIN/MAX): the
 * first batches read that many rows in all, then small ones. */
void
xstore_iscan_hint(xstore_iscan_t *s, int64_t nrows)
{
	if (s != NULL && s->ra == NULL)
		s->ra_left = nrows < 1 ? 1 : nrows > 1 << 20 ? 1 << 20 :
		    (int)nrows;
}

/* The next committed candidate from the row batch into s->ck / s->crid
 * (s->ra_i - 1 is its slot): 1, 0 at the end, -1 on an error. */
static int
xs_iscan_cand(xstore_iscan_t *s)
{
	int r, i;
	for (;;) {
		if (s->ra == NULL || s->ra_i >= s->ra_n) {
			if (s->ra_end) return 0;
			if ((r = xs_iscan_rows(s)) <= 0) {
				if (r == 0) s->ra_end = 1;
				return r;
			}
		}
		i = s->ra_i++;
		/* The buffered write decides (checked again here: a write
		 * since the batch was read must win, as it did per row). */
		if (s->cx != NULL && s->cx->in_txn &&
		    wbuf_find(s->cx, s->rt.tableid, s->ra->rid[i]) != NULL)
			continue;
		s->cklen = s->ra->klen[i];
		memcpy(s->ck, s->ra->kbuf + s->ra->koff[i], (size_t)s->cklen);
		s->crid = s->ra->rid[i];
		return 1;
	}
}

/* The next verified row in entry order (1), 0 at the end, -1 on an
 * error (s->err); s->lk / s->lkl is its entry key. */
static int
xs_iscan_next1(xstore_iscan_t *s, int64_t *rowid, const uint8_t **rec,
    int *reclen)
{
	uint8_t kk[XS_IDX_KMAX];
	const uint8_t *v;
	int c, rl, kl, vr, r;
	for (;;) {
		if (s->err != 0)
			return -1;
		if (s->done) return 0;
		if (!s->have_ck) {
			c = xs_iscan_cand(s);
			if (c < 0)
				return -1;
			s->have_ck = c ? 1 : -1;
		}
		/* Merge: take the buffered candidate when it sorts first. */
		if (s->bci < s->nbc) {
			const struct xs_icand *b = &s->bc[s->bci];
			if (s->have_ck == 1) {
				c = memcmp(b->key, s->ck, (size_t)(b->klen < s->cklen ? b->klen : s->cklen));
				if (c == 0) c = b->klen - s->cklen;
				if (s->reverse) c = -c;
			} else {
				c = -1;
			}
			if (c < 0) {
				const xs_wrec_t *bw =
				    wbuf_find(s->cx, s->rt.tableid, b->rowid);
				int wi = bw != NULL ? (int)(bw - s->cx->wbuf) : -1;
				const uint8_t *p;
				s->bci++;
				if (wi < 0 || bw->deleted)
					continue; /* deleted since the open */
				/* An unreadable spilled payload: an error. */
				r = xs_wrec_payload(s->cx, wi, &p, &rl);
				if (r != XTC_OK)
					return xs_iscan_fail(s, r);
				if (xs_iscan_rec(s) == NULL)
					return -1;
				if (rl > XS_VMAX) rl = XS_VMAX;
				memcpy(s->rec + 1, p, (size_t)rl);
				s->visited++;
				s->lk = b->key;
				s->lkl = b->klen;
				*rowid = b->rowid; *rec = s->rec + 1; *reclen = rl;
				return 1;
			}
		}
		if (s->have_ck != 1) { s->done = 1; return 0; }
		s->have_ck = 0;
		/* Verify: the candidate's visible version still carries this key. */
		s->visited++;
		vr = s->ra->rc[s->ra_i - 1];
		if (vr == 0) continue;            /* not visible at the snapshot */
		if (vr > 0) {
			v = s->ra->vbuf + s->ra->voff[s->ra_i - 1];
			rl = (int)s->ra->vlen[s->ra_i - 1] - 1;
			if (rl > XS_VMAX) rl = XS_VMAX;
		} else {
			if ((v = xs_iscan_rec(s)) == NULL)
				return -1;
			rl = xs_point_get(s->bt, s->rt.tableid, s->crid,
			    s->snap, s->rec, 1 + XS_VMAX, &s->err);
			if (s->err == 0 && xs_rd_fault())
				s->err = XTC_E_IO;
			if (s->err != 0) {
				(void)xs_rd_count(s->err);
				return -1;
			}
			if (rl == 0) continue;
			if (rl < 0) rl = 0;
		}
		kl = xs_idx_rowkey(&s->rt, v + 1, rl, s->crid, 1, kk,
		    (int)sizeof kk, NULL);
		if (kl != s->cklen || memcmp(kk, s->ck, (size_t)kl) != 0)
			continue;                     /* stale entry */
		if (!xs_iscan_row_ok(s, v + 1, rl, s->crid))
			continue; /* in a cut bound's group only */
		if (s->cx != NULL && s->cx->in_txn && s->cx->serializable) {
			rset_add(s->cx, s->rt.tableid, s->crid);
			ssi_record_read(s->cx->ssi, s->rt.tableid, s->crid);
		}
		s->lk = s->ck;
		s->lkl = s->cklen;
		*rowid = s->crid; *rec = v + 1; *reclen = rl;
		return 1;
	}
}

/* The next row in entry order: the one held past a group, else
 * xs_iscan_next1.  *k / *kl is its entry key. */
static int
xs_iscan_take(xstore_iscan_t *s, int64_t *rowid, const uint8_t **rec,
    int *reclen, const uint8_t **k, int *kl)
{
	struct xs_igrp *g = s->grp;
	int r;
	if (g != NULL && g->have_next) {
		g->have_next = 0;
		*rowid = g->next_rid;
		*rec = g->next_rec;
		*reclen = g->next_rl;
		*k = g->next_k;
		*kl = g->next_kl;
		return 1;
	}
	r = xs_iscan_next1(s, rowid, rec, reclen);
	if (r == 1) {
		*k = s->lk;
		*kl = s->lkl;
	}
	return r;
}

/* Append a row to the group: its record and its whole tuple. */
static int
xs_igrp_add(xstore_iscan_t *s, int64_t rid, const uint8_t *rec, int rl)
{
	struct xs_igrp *g = s->grp;
	struct xs_igrow *w;
	uint8_t *b;
	int tl;
	if (g->n == g->cap) {
		int nc = g->cap ? g->cap * 2 : 8;
		struct xs_igrow *nr = realloc(g->row, sizeof *nr * (size_t)nc);
		if (nr == NULL)
			return -1;
		g->row = nr;
		g->cap = nc;
	}
	if ((b = malloc((size_t)rl + XS_IDX_TMAX)) == NULL)
		return -1;
	memcpy(b, rec, (size_t)rl);
	tl = xs_idx_tuple(&s->rt, rec, rl, rid, b + rl, XS_IDX_TMAX, NULL);
	w = &g->row[g->n++];
	w->rid = rid;
	w->rl = rl;
	w->tl = tl < 0 ? 0 : tl;
	w->buf = b;
	return 0;
}

static int
xs_igrow_cmp(const void *pa, const void *pb)
{
	const struct xs_igrow *a = pa, *b = pb;
	int r = memcmp(a->buf + a->rl, b->buf + b->rl,
	    (size_t)(a->tl < b->tl ? a->tl : b->tl));
	if (r != 0)
		return r;
	if (a->tl != b->tl)
		return a->tl < b->tl ? -1 : 1;
	return a->rid < b->rid ? -1 : a->rid > b->rid;
}

/*
 * The row just taken (rid, rec, its truncated entry key k) starts a
 * group: read the rest of the group (every following row whose entry is
 * truncated with the same first P tuple bytes), hold the first row past
 * it, sort the group by whole tuple and rowid (descending for a reverse
 * scan), and return its first row.  Groups are contiguous in entry
 * order: a whole key never shares a truncated key's first P bytes.
 */
static int
xs_iscan_group(xstore_iscan_t *s, int64_t *rowid, const uint8_t **rec,
    int *reclen, const uint8_t *k)
{
	struct xs_igrp *g = s->grp;
	uint8_t gk[XS_IDX_KMAX];
	int gl = s->rt.kmax - 16, i, r, rl, kl;
	int64_t rid;
	const uint8_t *rc, *kp;
	if (g == NULL && (g = s->grp = calloc(1, sizeof *g)) == NULL)
		return -1;
	for (i = 0; i < g->n; i++)
		free(g->row[i].buf);
	g->n = g->i = 0;
	memcpy(gk, k, (size_t)gl);
	if (xs_igrp_add(s, *rowid, *rec, *reclen) != 0)
		return -1;
	for (;;) {
		r = xs_iscan_take(s, &rid, &rc, &rl, &kp, &kl);
		if (r < 0)
			return -1;
		if (r == 0)
			break;
		if (xs_idx_truncated(&s->rt, kl) &&
		    memcmp(kp, gk, (size_t)gl) == 0) {
			if (xs_igrp_add(s, rid, rc, rl) != 0)
				return -1;
			continue;
		}
		g->have_next = 1; /* past the group: hold it */
		g->next_rid = rid;
		g->next_rl = rl;
		g->next_kl = kl;
		memcpy(g->next_rec, rc, (size_t)rl);
		memcpy(g->next_k, kp, (size_t)kl);
		break;
	}
	qsort(g->row, (size_t)g->n, sizeof *g->row, xs_igrow_cmp);
	if (s->reverse)
		for (i = 0; i < g->n / 2; i++) {
			struct xs_igrow t = g->row[i];
			g->row[i] = g->row[g->n - 1 - i];
			g->row[g->n - 1 - i] = t;
		}
	g->i = 1;
	*rowid = g->row[0].rid;
	*rec = g->row[0].buf;
	*reclen = g->row[0].rl;
	return 1;
}

/* Leave the scan's error in the connection's rd_err (the first one of
 * the statement), for the engine to report (xstore_read_err).  A group
 * allocation failure is the one error without s->err. */
static void
xs_iscan_err_msg(xstore_iscan_t *s)
{
	struct xs_idx_ent e;
	int rc = s->err != 0 ? s->err : XTC_E_NOMEM;
	if (s->cx == NULL || s->cx->rd_err[0] != '\0' || s->cx->rd_rc != 0)
		return;
	if (xs_idx_get(s->bt, s->rt.idx_id, &e))
		snprintf(s->cx->rd_err, sizeof s->cx->rd_err,
		    "index error: cannot read index %s on %s: %s", e.name,
		    e.table, xtc_strerror(rc));
	else
		snprintf(s->cx->rd_err, sizeof s->cx->rd_err,
		    "index error: cannot read an index: %s", xtc_strerror(rc));
}

int
xstore_iscan_next(xstore_iscan_t *s, int64_t *rowid, const uint8_t **rec,
    int *reclen)
{
	struct xs_igrp *g;
	const uint8_t *k;
	int r, kl;
	if (s == NULL)
		return -1;
	g = s->grp;
	if (g != NULL && g->i < g->n) {
		*rowid = g->row[g->i].rid;
		*rec = g->row[g->i].buf;
		*reclen = g->row[g->i].rl;
		g->i++;
		return 1;
	}
	r = xs_iscan_take(s, rowid, rec, reclen, &k, &kl);
	if (r == 1 && xs_idx_truncated(&s->rt, kl))
		r = xs_iscan_group(s, rowid, rec, reclen, k);
	if (r < 0)
		xs_iscan_err_msg(s);
	return r;
}

void
xstore_iscan_close(xstore_iscan_t *s)
{
	if (s == NULL) return;
	if (s->visited)
		xtc_atomic_fetch_add_u64(&g_rows_visited, s->visited,
		    xtc_mo_relaxed);
	if (s->cur != NULL) bt_cursor_close(s->cur);
	free(s->ka);
	free(s->ra);
	free(s->rec);
	free(s->bc);
	free(s->bkeys);
	free(s->fb);
	if (s->grp != NULL) {
		int i;
		for (i = 0; i < s->grp->n; i++)
			free(s->grp->row[i].buf);
		free(s->grp->row);
		free(s->grp);
	}
	free(s);
}

int
xstore_drop_table(struct xsql *db, const char *name)
{
	bt_t *bt = xstore_bt_of(db);
	uint32_t id, top;
	uint8_t vbuf[256];
	size_t nlen;
	int err;
	xs_cat_gen_bump();
	if (bt == NULL || name == NULL) return -1;
	id = xs_cat_lookup(bt, name, &err);
	if (err != 0) {                      /* unread: not known to be gone */
		xstore_read_err_catalog(db, err);
		return -1;
	}
	if (id == 0) return 0;               /* not present: idempotent */
	/* Tombstone the catalog row: a new version at (XS_CAT_TABLEID, id)
	 * marked deleted, so xs_cat_lookup skips it.  Keep the name as the
	 * payload (harmless; the delete flag is what matters). */
	nlen = strlen(name);
	if (nlen > sizeof vbuf) nlen = sizeof vbuf;
	memcpy(vbuf, name, nlen);
	/* The marks first: once GC takes the tombstone, it alone keeps the
	 * id from coming back (see the table-id comment).  The index mark
	 * too, before the table goes: a DROP that cannot raise it fails
	 * whole, rather than leave live index rows naming a dropped table
	 * (a new table of the name would inherit them at the next open). */
	top = xs_idx_table_top(bt, name);
	if ((err = xs_hw_raise(bt, XS_CAT_TABLEID, id)) != SX_OK ||
	    (top != 0 && (err = xs_hw_raise(bt, XS_IDXCAT_TABLEID, top)) !=
	    SX_OK)) {
		if (err < -1)
			xstore_read_err_catalog(db, err);
		return -1;
	}
	if (xs_drop_gap("xstore.drop.table_gap") ||
	    xs_put(bt, XS_CAT_TABLEID, (int64_t)id, vbuf, (int)nlen, 1) != SX_OK)
		return -1;
	xs_idx_drop_table(bt, name);
	xstore_cat_forget_one(bt, name);
	return 0;
}

/* ALTER TABLE oldname RENAME TO newname: re-key the catalog entry to the
 * new name, keeping the same table-id (and therefore all the row data)
 * and column schema.  Returns 0 on success, 0 (idempotent no-op) if the
 * old name is unknown, -1 on error or if the new name already exists. */
int
xstore_rename_table(struct xsql *db, const char *oldname, const char *newname)
{
	bt_t *bt = xstore_bt_of(db);
	uint32_t id, exists = 0;
	uint8_t val[1024], vbuf[1024];
	int vlen, i, err;
	const char *coldefs = NULL;
	size_t nlen, clen = 0, total;
	xs_cat_gen_bump();
	if (bt == NULL || oldname == NULL || newname == NULL) return -1;
	id = xs_cat_lookup(bt, oldname, &err);
	if (id == 0 && err == 0) return 0;           /* unknown: idempotent */
	if (err == 0)
		exists = xs_cat_lookup(bt, newname, &err);
	if (err != 0) {                              /* unread */
		xstore_read_err_catalog(db, err);
		return -1;
	}
	if (exists != 0 && exists != id) return -1;  /* target name taken */

	/* Recover the column schema from the persisted catalog value
	 * (name\0coldefs); fall back to the cached coldefs.  A value that
	 * cannot be read fails the rename: a new row without the schema
	 * would lose the table's columns. */
	vlen = xs_cat_value(bt, oldname, val, (int)sizeof val - 1);
	if (vlen < 0) {
		xstore_read_err_catalog(db, vlen);
		return -1;
	}
	if (vlen > 0) {
		val[vlen] = '\0';      /* the value is not NUL-terminated */
		for (i = 0; i < vlen; i++) if (val[i] == 0) break;
		if (i < vlen - 1) coldefs = (const char *)val + i + 1;
	}
	if (coldefs == NULL) {
		xs_cat_lock();
		for (i = 0; i < g_cat_n; i++)
			if (g_cat[i].bt == bt && strcasecmp(g_cat[i].name, oldname) == 0 &&
			    g_cat[i].coldefs[0] != '\0') { coldefs = g_cat[i].coldefs; break; }
		xs_cat_unlock();
	}

	/* Persist a new catalog row for the SAME table-id under the new
	 * name (name\0coldefs), so xs_cat_lookup(newname) resolves to id and
	 * xs_cat_lookup(oldname) no longer matches (one row per table-id). */
	nlen = strlen(newname);
	clen = coldefs ? strlen(coldefs) : 0;
	if (nlen + 1 + clen > sizeof vbuf) clen = 0;
	memcpy(vbuf, newname, nlen);
	total = nlen;
	if (clen > 0) { vbuf[total++] = '\0'; memcpy(vbuf + total, coldefs, clen); total += clen; }
	if (xs_put(bt, XS_CAT_TABLEID, (int64_t)id, vbuf, (int)total, 0) != SX_OK)
		return -1;

	/* Update the in-process cache: drop the old name, install the new
	 * (same id + coldefs) so cross-connection lookups see it at once. */
	xs_cat_lock();
	for (i = 0; i < g_cat_n; i++)
		if (g_cat[i].bt == bt && strcasecmp(g_cat[i].name, oldname) == 0) {
			snprintf(g_cat[i].name, sizeof g_cat[i].name, "%s", newname);
			XS_CATC_BUMP();
			if (clen > 0)
				snprintf(g_cat[i].coldefs, sizeof g_cat[i].coldefs, "%s", coldefs);
			break;
		}
	xs_cat_unlock();
	xs_idx_rename_table(bt, oldname, newname);
	return 0;
}

/* SQL functions: xstore_now() -> current clock; xstore_as_of(ts) pins
 * this connection's read snapshot (0 = latest) and returns it. */
#if defined(SQLXTC_VTAB)
static void
fn_now(xsql_context *ctx, int argc, xsql_value **argv)
{
	(void)argc; (void)argv;
	xsql_result_int64(ctx,
	    (xsql_int64)xtc_atomic_load_u64(&g_xclock, xtc_mo_relaxed));
}
#endif /* SQLXTC_VTAB */
#if defined(SQLXTC_VTAB)
static void
fn_as_of(xsql_context *ctx, int argc, xsql_value **argv)
{
	xstore_ctx_t *c = (xstore_ctx_t *)xsql_user_data(ctx);
	int64_t ts = (argc >= 1) ? xsql_value_int64(argv[0]) : 0;
	if (ts < 0) ts = 0;
	xtc_atomic_store_u64(&c->read_snap, (uint64_t)ts, xtc_mo_relaxed);
	xs_pin_recompute(c);      /* pin (or release) the as_of snapshot */
	xsql_result_int64(ctx, ts);
}
#endif /* SQLXTC_VTAB */
/* xstore_serializable(on): set this connection's isolation to
 * serializable (on != 0) or snapshot isolation (0).  Returns the new
 * setting.  Serializable validates the transaction's read set at
 * commit and aborts (SX_BUSY) on a read-write conflict. */
#if defined(SQLXTC_VTAB)
static void
fn_serializable(xsql_context *ctx, int argc, xsql_value **argv)
{
	xstore_ctx_t *c = (xstore_ctx_t *)xsql_user_data(ctx);
	int on = (argc >= 1) ? (xsql_value_int(argv[0]) != 0) : 1;
	c->serializable = on;
	c->isolation = on ? XS_ISO_SERIALIZABLE : XS_ISO_SNAPSHOT;
	xsql_result_int(ctx, on);
}
#endif /* SQLXTC_VTAB */

/* Case-insensitive ASCII prefix match: 1 if `s` starts with `pfx`. */
static int
ciprefix(const char *s, const char *pfx)
{
	size_t i;
	for (i = 0; pfx[i] != '\0'; i++) {
		int a = (unsigned char)s[i], b = (unsigned char)pfx[i];
		if (a >= 'A' && a <= 'Z') a += 32;
		if (b >= 'A' && b <= 'Z') b += 32;
		if (a != b)
			return 0;
	}
	return 1;
}

/* xstore_isolation(level): set this connection's isolation level.  The
 * argument is a name ("read uncommitted", "read committed", "repeatable
 * read", "snapshot", "serializable", case-insensitive) or the integer
 * XS_ISO_* code.  Returns the resulting integer level.  Covers the
 * PostgreSQL/SQL-standard set plus snapshot; takes effect at the next
 * transaction's first access. */
#if defined(SQLXTC_VTAB)
static void
fn_isolation(xsql_context *ctx, int argc, xsql_value **argv)
{
	xstore_ctx_t *c = (xstore_ctx_t *)xsql_user_data(ctx);
	int lvl = c->isolation;

	if (argc >= 1) {
		int t = xsql_value_type(argv[0]);
		if (t == SQLITE_INTEGER) {
			int v = xsql_value_int(argv[0]);
			if (v >= XS_ISO_READ_UNCOMMITTED &&
			    v <= XS_ISO_SERIALIZABLE)
				lvl = v;
		} else {
			const char *s = (const char *)xsql_value_text(argv[0]);
			if (s != NULL) {
				if (ciprefix(s, "read un"))
					lvl = XS_ISO_READ_UNCOMMITTED;
				else if (ciprefix(s, "read c"))
					lvl = XS_ISO_READ_COMMITTED;
				else if (ciprefix(s, "rep"))
					lvl = XS_ISO_REPEATABLE_READ;
				else if (ciprefix(s, "snap"))
					lvl = XS_ISO_SNAPSHOT;
				else if (ciprefix(s, "ser"))
					lvl = XS_ISO_SERIALIZABLE;
			}
		}
		c->isolation = lvl;
		c->serializable = (lvl == XS_ISO_SERIALIZABLE);
	}
	xsql_result_int(ctx, lvl);
}
#endif /* SQLXTC_VTAB */
/* xstore_gc(): vacuum dead versions up to the current GC horizon (the
 * oldest live snapshot, or the clock if none).  Returns the number of
 * versions reclaimed.  The horizon advances g_gc_horizon so an as_of
 * read below it is known to be best-effort. */
#if defined(SQLXTC_VTAB)
static void
fn_gc(xsql_context *ctx, int argc, xsql_value **argv)
{
	xstore_ctx_t *c = (xstore_ctx_t *)xsql_user_data(ctx);
	uint64_t now = xtc_atomic_load_u64(&g_xclock, xtc_mo_relaxed);
	uint64_t horizon = snap_horizon(now);
	int reclaimed = 0;
	(void)argc; (void)argv;
	(void)xs_gc(c->bt, horizon, &reclaimed);
	if (horizon > xtc_atomic_load_u64(&g_gc_horizon, xtc_mo_relaxed))
		xtc_atomic_store_u64(&g_gc_horizon, horizon, xtc_mo_relaxed);
	xsql_result_int(ctx, reclaimed);
}
#endif /* SQLXTC_VTAB */

/* xstore_autovacuum(on): when on (default off), each write prunes that
 * rowid's dead versions inline (HOT-style), keeping version chains
 * short on a write-heavy workload without a full-tree scan.  Off keeps
 * all versions, so xstore_as_of() time travel can reach any historical
 * snapshot; on bounds time travel to the live-snapshot horizon. */
#if defined(SQLXTC_VTAB)
static void
fn_autovacuum(xsql_context *ctx, int argc, xsql_value **argv)
{
	xstore_ctx_t *c = (xstore_ctx_t *)xsql_user_data(ctx);
	int on = (argc >= 1) ? (xsql_value_int(argv[0]) != 0) : 1;
	c->autovacuum = on;
	xsql_result_int(ctx, on);
}
#endif /* SQLXTC_VTAB */
/* xstore_prune_count(): number of inline-prune passes adaptive
 * autovacuum has actually run (vs the number of writes) -- lets a test
 * see the backoff on a low-garbage workload. */
#if defined(SQLXTC_VTAB)
static void
fn_prune_count(xsql_context *ctx, int argc, xsql_value **argv)
{
	(void)argc; (void)argv;
	xsql_result_int64(ctx,
	    (xsql_int64)xtc_atomic_load_u64(&g_av_prunes, xtc_mo_relaxed));
}
#endif /* SQLXTC_VTAB */

/*
 * ALTER TABLE ... RENAME TO newname.  The table keeps its id; we write
 * a persisted catalog override (table-id 0, rowid = id, value = the new
 * name) so a later connect resolves the new name to the same id and the
 * data is preserved.  Goes through xs_put, so the override is WAL-
 * logged and recovered.  The in-process cache is refreshed so this
 * connection's other handles agree immediately.
 */
#if defined(SQLXTC_VTAB)
static int
xs_rename(xsql_vtab *pv, const char *newname)
{
	xstore_vtab_t *v = (xstore_vtab_t *)pv;
	uint32_t id;
	int i;
	if (newname == NULL) return SX_OK;
	id = v->tableid;
	xs_cat_lock();
	for (i = 0; i < g_cat_n; i++)
		if (g_cat[i].bt == v->ctx->bt && g_cat[i].tableid == id) {
			snprintf(g_cat[i].name, sizeof g_cat[i].name, "%s", newname);
			XS_CATC_BUMP();
			break;
		}
	xs_cat_unlock();
	/* New version of the same id's catalog row -- outside the lock, since
	 * xs_put can park on the WAL ack (see xs_cat_find_or_create). */
	(void)xs_put(v->ctx->bt, XS_CAT_TABLEID, (int64_t)id,
	    newname, (int)strlen(newname), 0);
	return SX_OK;
}
#endif /* SQLXTC_VTAB */

#if defined(SQLXTC_VTAB)
static const xsql_module xstore_module = {
	.iVersion    = 2,
	.xCreate     = xs_connect,
	.xConnect    = xs_connect,
	.xBestIndex  = xs_best_index,
	.xDisconnect = xs_disconnect,
	.xDestroy    = xs_disconnect,
	.xOpen       = xs_open,
	.xClose      = xs_close,
	.xFilter     = xs_filter,
	.xNext       = xs_next,
	.xEof        = xs_eof,
	.xColumn     = xs_column,
	.xRowid      = xs_rowid,
	.xUpdate     = xs_update,
	.xBegin      = xs_begin,
	.xSync       = xs_sync,
	.xCommit     = xs_commit,
	.xRollback   = xs_rollback,
	.xRename     = xs_rename,
	.xSavepoint  = xs_savepoint,
	.xRelease    = xs_release,
	.xRollbackTo = xs_rollback_to,
};
#endif /* SQLXTC_VTAB */

static void
ctx_free(void *p)
{
	xstore_ctx_t *cx = p;
	if (cx != NULL) { ssi_abort(cx->ssi); snap_slot_free(&cx->snap_slot);
	    wbuf_clear(cx); xs_ilk_slot_free(cx); free(cx->ilk); free(cx->wbuf);
	    free(cx->rset);
	    free(cx->sp_wn); free(cx->sp_rn); free(cx->wmap); free(cx->umap); }
	free(p);
}

/* Connection commit/rollback hooks: close a READ-ONLY transaction at
 * its end.  A read-only transaction buffers no writes and never fires
 * the vtab's write-path xCommit/xRollback, so without this its in_txn
 * would persist and freeze txn_snap across later transactions.  When
 * writes ARE buffered, the vtab 2PC path (xs_sync/xs_commit or
 * xs_rollback) owns the transaction and has already reset in_txn by
 * the time SQLite reaches the connection-level commit; in that case
 * (wn != 0 was flushed, in_txn already 0) this is a no-op.  Guard on
 * the absence of pending writes so the hook never discards a write
 * set out from under the 2PC flush. */
#if defined(SQLXTC_VTAB)
static int
xs_conn_commit(void *p)
{
	xstore_ctx_t *cx = p;
	if (cx->in_txn && cx->wn == 0) {   /* read-only transaction ending */
		if (cx->ssi != NULL) { ssi_abort(cx->ssi); cx->ssi = NULL; }
		cx->in_txn = 0;
		xs_pin_recompute(cx);
	}
	return 0;   /* allow the commit */
}
#endif /* SQLXTC_VTAB */

#if defined(SQLXTC_VTAB)
static void
xs_conn_rollback(void *p)
{
	xstore_ctx_t *cx = p;
	if (cx->in_txn && cx->wn == 0) {   /* read-only transaction rolling back */
		if (cx->ssi != NULL) { ssi_abort(cx->ssi); cx->ssi = NULL; }
		cx->in_txn = 0;
		xs_pin_recompute(cx);
	}
}
#endif /* SQLXTC_VTAB */

/* Allocate a connection ctx, initialise it, and record the
 * connection -> (bt, ctx) association in g_dbmap.  Shared by the SQLite
 * registration (which then also installs the vtab module + SQL
 * functions) and the native registration (which needs nothing else).
 * Returns the ctx, or NULL on OOM.  In native mode autocommit is owned
 * by the engine from the start. */
static xstore_ctx_t *
xs_ctx_init_and_map(struct xsql *db, bt_t *bt, int native)
{
	xstore_ctx_t *ctx = calloc(1, sizeof *ctx);
	int i, slot = -1;
	if (ctx == NULL)
		return NULL;
	bt_set_close_hook(xstore_cat_forget);   /* clear cached ids when a bt closes */
	ctx->bt = bt;
	xs_cat_lock();
	for (i = 0; i < g_dbmap_n; i++)
		if (g_dbmap[i].db == db) { slot = i; break; }
	if (slot < 0 && g_dbmap_n < XS_DBMAP_MAX) slot = g_dbmap_n++;
	if (slot >= 0) { g_dbmap[slot].db = db; g_dbmap[slot].bt = bt; g_dbmap[slot].ctx = ctx; }
	xtc_atomic_fetch_add_u64(&g_dbmap_gen, 1, xtc_mo_release);
	xs_cat_unlock();
	if (slot < 0) {
		/* Map full: refuse the connection rather than hand back a
		 * handle with no transaction context. */
		free(ctx);
		return NULL;
	}
	/* Zeroed by calloc: every field not set here starts at 0 / NULL
	 * (no transaction, empty buffers and maps, no read error), so a new
	 * field is zero unless it is listed below. */
	ctx->isolation = XS_ISO_SNAPSHOT;
	ctx->txn_iso = XS_ISO_SNAPSHOT;
	ctx->snap_slot = -1;
	ctx->autovacuum = xtc_atomic_load_i32(&g_av_default, xtc_mo_relaxed);
	ctx->native_mode = native ? 1 : 0;
	ctx->native_autocommit = 1;
	ctx->wmax_ok = 1;
	return ctx;
}

/*
 * Native registration: a sqlite3-free connection.  Installs only the
 * ctx + g_dbmap mapping (no vtab module, no SQL functions, no
 * commit/rollback hooks -- the native engine owns transactions and
 * exposes no SQL UDFs).  The connection runs in native_mode from the
 * start.
 */
int
xstore_register_native(struct xsql *db, bt_t *bt)
{
	xstore_ctx_t *ctx = xs_ctx_init_and_map(db, bt, 1);
	if (ctx == NULL)
		return SX_NOMEM;
	return SX_OK;
}

#if defined(SQLXTC_VTAB)
int
xstore_register(xsql *db, bt_t *bt)
{
	xstore_ctx_t *ctx = xs_ctx_init_and_map(db, bt, 0);
	int rc;
	if (ctx == NULL)
		return SX_NOMEM;
	rc = xsql_create_module_v2(db, "xstore", &xstore_module, ctx, ctx_free);
	if (rc != SX_OK)
		return rc;
	/* End-of-transaction hooks so read-only transactions reset in_txn
	 * (they fire no vtab write hook); the ctx outlives them via the
	 * module's ctx_free. */
	(void)xsql_commit_hook(db, xs_conn_commit, ctx);
	(void)xsql_rollback_hook(db, xs_conn_rollback, ctx);
	(void)xsql_create_function(db, "xstore_now", 0,
	    SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, fn_now, NULL, NULL);
	(void)xsql_create_function(db, "xstore_as_of", 1, SQLITE_UTF8,
	    ctx, fn_as_of, NULL, NULL);
	(void)xsql_create_function(db, "xstore_serializable", 1, SQLITE_UTF8,
	    ctx, fn_serializable, NULL, NULL);
	(void)xsql_create_function(db, "xstore_isolation", 1, SQLITE_UTF8,
	    ctx, fn_isolation, NULL, NULL);
	(void)xsql_create_function(db, "xstore_gc", 0, SQLITE_UTF8,
	    ctx, fn_gc, NULL, NULL);
	(void)xsql_create_function(db, "xstore_autovacuum", 1, SQLITE_UTF8,
	    ctx, fn_autovacuum, NULL, NULL);
	(void)xsql_create_function(db, "xstore_prune_count", 0, SQLITE_UTF8,
	    ctx, fn_prune_count, NULL, NULL);
	return SX_OK;
}
#endif /* SQLXTC_VTAB */

/* Drop the connection -> bt/ctx association when a connection closes, so
 * the fixed-size g_dbmap does not leak slots and overflow under many
 * short-lived connections (the 64-client load test).  The ctx itself is
 * freed by SQLite's module ctx_free; here we only release the map slot
 * (swap-remove).  Without this, the (N+1)-th connection past XS_DBMAP_MAX
 * went unregistered, so xstore_bt_of returned NULL and every native
 * statement on it fell back to the VDBE -- which does not know the
 * catalog-only tables ("no such table").  Safe to call for a handle that
 * was never registered. */
void
xstore_unregister(struct xsql *db)
{
	int i;
	xs_cat_lock();
	for (i = 0; i < g_dbmap_n; i++)
		if (g_dbmap[i].db == db) {
			g_dbmap_n--;
			if (i != g_dbmap_n) g_dbmap[i] = g_dbmap[g_dbmap_n];
			xtc_atomic_fetch_add_u64(&g_dbmap_gen, 1,
			    xtc_mo_release);
			break;
		}
	xs_cat_unlock();
}

/*
 * Native-handle unregister: like xstore_unregister, but also FREES the
 * connection ctx (a native sx_open has no SQLite xsql_close to invoke
 * the module's ctx_free, so the ctx is freed here).  Used only by a
 * native (sqlite3-free) connection on close.
 */
void
xstore_unregister_native(struct xsql *db)
{
	xstore_ctx_t *ctx = NULL;
	int i;
	xs_cat_lock();
	for (i = 0; i < g_dbmap_n; i++)
		if (g_dbmap[i].db == db) {
			ctx = g_dbmap[i].ctx;
			g_dbmap_n--;
			if (i != g_dbmap_n) g_dbmap[i] = g_dbmap[g_dbmap_n];
			xtc_atomic_fetch_add_u64(&g_dbmap_gen, 1,
			    xtc_mo_release);
			break;
		}
	xs_cat_unlock();
	if (ctx != NULL)
		ctx_free(ctx);
}

/*
 * Recovery driver.  One forward scan over the log redoes every update
 * (idempotent: version keys (rowid, ~commit_ts) are immutable) and, as
 * it goes, tracks the still-active transactions in a small table.  A
 * transaction is born at its first XL_UPDATE and retired at its
 * XL_COMMIT; whatever is still active when the log ends is a LOSER --
 * its updates reached the tree but it never committed -- and is undone
 * newest-first, a compensation record (XL_CLR) written per reversed
 * update so a crash during recovery resumes without redoing compensated
 * work, then an XL_END.
 *
 * In the NO-STEAL commit path a transaction's whole frame is
 * [XL_UPDATE...][XL_COMMIT], so it is retired in the same frame it is
 * born and the active table is empty between frames -- no losers, the
 * undo pass is skipped.  Losers arise only once uncommitted updates can
 * reach the tree before commit (STEAL); the machinery is exercised by
 * test_recover_undo.  Advances the commit clock past the highest
 * recovered timestamp so new commits do not collide with recovered ones.
 */
struct rec_upd {
	uint64_t lsn;            /* WAL LSN of the frame this update arrived in */
	uint32_t tableid;
	int64_t  rowid;
	uint64_t commit_ts;
};
struct rec_txn {
	uint64_t        txn_id;
	struct rec_upd *upd;
	int             n_upd, cap_upd;
};
struct xs_recover {
	bt_t    *bt;
	bm_t    *bm;             /* non-NULL for in-place mode: apply XL_PAGE */
	int      page_pass;      /* in-place pass 1: apply ONLY XL_PAGE images,
	                          * skip logical redo (see xstore_recover_inplace) */
	uint64_t max_ts;
	uint64_t records;
	uint64_t pages_redone;   /* XL_PAGE images applied (in-place mode) */
	uint64_t redo_start;     /* fuzzy checkpoint horizon: skip logical redo
	                          * of records below this LSN (already durable
	                          * on the trusted base); 0 == replay from front */
	struct rec_txn *txn;     /* ACTIVE (uncommitted) transactions only */
	int      n_txn, cap_txn;
	int rc; /* why the replay stopped (XTC_OK: it did not) */
};

static struct rec_txn *
rec_txn_get(struct xs_recover *r, uint64_t txn_id)
{
	int i;
	struct rec_txn *t;

	for (i = 0; i < r->n_txn; i++)
		if (r->txn[i].txn_id == txn_id)
			return &r->txn[i];
	if (r->n_txn == r->cap_txn) {
		int nc = r->cap_txn ? r->cap_txn * 2 : 8;
		struct rec_txn *nt = realloc(r->txn, (size_t)nc * sizeof *nt);
		if (nt == NULL)
			return NULL;
		r->txn = nt; r->cap_txn = nc;
	}
	t = &r->txn[r->n_txn++];
	t->txn_id = txn_id; t->upd = NULL; t->n_upd = 0; t->cap_upd = 0;
	return t;
}

static int
rec_txn_addupd(struct rec_txn *t, uint64_t lsn, uint32_t tableid, int64_t rowid,
    uint64_t commit_ts)
{
	if (t->n_upd == t->cap_upd) {
		int nc = t->cap_upd ? t->cap_upd * 2 : 8;
		struct rec_upd *nu = realloc(t->upd, (size_t)nc * sizeof *nu);
		if (nu == NULL)
			return XTC_E_NOMEM; /* a loser could not be undone */
		t->upd = nu; t->cap_upd = nc;
	}
	t->upd[t->n_upd].lsn = lsn;
	t->upd[t->n_upd].tableid = tableid;
	t->upd[t->n_upd].rowid = rowid;
	t->upd[t->n_upd].commit_ts = commit_ts;
	t->n_upd++;
	return XTC_OK;
}

/* Retire a committed (winner) transaction: its redo stays, drop it from
 * the active table by swap-remove. */
static void
rec_txn_retire(struct xs_recover *r, struct rec_txn *t)
{
	int idx = (int)(t - r->txn);
	free(t->upd);
	r->txn[idx] = r->txn[--r->n_txn];
}

/* Undo one loser newest-first: delete each version it inserted and write
 * an XL_CLR naming the next LSN still to undo (0 when done), then an
 * XL_END.  A CLR is redo-only and never itself undone.  Returns XTC_OK,
 * or the first failure (the loser is then not fully undone). */
static int
xs_undo_loser(bt_t *bt, wal_t *w, struct rec_txn *t)
{
	uint8_t end[XL_HDR_LEN];
	xl_hdr_t eh;
	uint64_t elsn;
	int i, n, rc;

	for (i = t->n_upd - 1; i >= 0; i--) {
		struct rec_upd *u = &t->upd[i];
		uint8_t key[XS_VKLEN];
		uint8_t clr[64];
		xl_hdr_t h;
		xl_body_t b;
		uint64_t lsn;

		enc_vkey(u->tableid, u->rowid, u->commit_ts, key);
		rc = bt_delete(bt, key, XS_VKLEN); /* reverse the insert */
		if (rc != XTC_OK && rc != XTC_E_NOTFOUND)
			return rc;

		h.type = XL_CLR; h.txn_id = t->txn_id; h.prev_lsn = u->lsn;
		memset(&b, 0, sizeof b);
		b.undo_next_lsn = (i > 0) ? t->upd[i - 1].lsn : 0;
		b.tableid = u->tableid; b.rowid = u->rowid; b.commit_ts = u->commit_ts;
		if ((n = xl_enc_clr(clr, sizeof clr, &h, &b)) <= 0)
			return XTC_E_INTERNAL;
		if ((rc = wal_commit_sync(w, clr, (uint32_t)n, &lsn)) != XTC_OK)
			return rc;
		xtc_atomic_fetch_add_u64(&g_undo_clrs, 1, xtc_mo_relaxed);
	}
	eh.type = XL_END;
	eh.txn_id = t->txn_id;
	eh.prev_lsn = 0;
	if ((n = xl_enc_simple(end, sizeof end, &eh)) <= 0)
		return XTC_E_INTERNAL;
	return wal_commit_sync(w, end, (uint32_t)n, &elsn);
}

static int
xs_recover_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	struct xs_recover *r = user;
	const uint8_t *base = rec;
	uint32_t off = 0;
	int rc;

	/*
	 * A WAL frame carries one transaction: a run of XL_UPDATE records
	 * then XL_COMMIT, a lone XL_CHECKPOINT, or (under STEAL) part of an
	 * in-flight transaction.  Walk its records.
	 *
	 * In-place recovery runs this in TWO passes (page_pass): pass 1
	 * (page_pass == 1) applies ONLY the XL_PAGE after-images -- so every
	 * torn NON-split leaf is repaired from its image BEFORE pass 2's
	 * logical redo descends it -- and skips the logical work; pass 2
	 * (page_pass == 0) does the logical XL_UPDATE redo / winner retire
	 * over the now-repaired base and skips XL_PAGE (already applied).
	 * Logical rebuild mode (r->bm == NULL) is single-pass with
	 * page_pass == 0 and no XL_PAGE apply.
	 */
	while (off < len) {
		xl_hdr_t h;
		int rl = xl_record_len(base + off, len - off);

		if (rl <= 0)
			break;                    /* malformed: stop on this frame */
		if (xl_parse_hdr(base + off, (uint32_t)rl, &h) != XTC_OK)
			break;
		if (r->page_pass) {
			/*
			 * Pass 1 (in-place only): physiological redo.  Write each
			 * full-page after-image onto its page ONLY if the on-disk
			 * page LSN is older (bm_apply_page_image is page-LSN gated,
			 * hence idempotent), repairing a torn structure or a torn
			 * NON-split leaf on the trusted base in place BEFORE any
			 * logical redo descends it.
			 */
			if (h.type == XL_PAGE && r->bm != NULL) {
				xl_hdr_t ph;
				uint32_t pid = 0;
				const void *image = NULL;
				uint16_t image_len = 0;
				/* Gate on the FRAME's LSN, not the LSN embedded in the
				 * image bytes: each image is its own monotonically-
				 * numbered WAL frame, so gating on the frame LSN makes
				 * the LAST image of a page win even when several images
				 * of one page share an embedded (commit) LSN. */
				if (xl_parse_page(base + off, (uint32_t)rl, &ph, &pid,
				    &image, &image_len) == XTC_OK &&
				    bm_apply_page_image_at(r->bm, pid, image, image_len,
				    lsn) == 1)
					r->pages_redone++;
			}
			off += (uint32_t)rl;
			continue;
		}
		if (h.type == XL_CHECKPOINT) {
			uint64_t clk, start = 0;
			if (xl_parse_checkpoint(base + off, (uint32_t)rl, &clk,
			    &start) == XTC_OK) {
				if (clk > r->max_ts)
					r->max_ts = clk;
				/*
				 * Fuzzy checkpoint (start_lsn != 0): the base is
				 * durable through the recLSN horizon this record
				 * carries, so every change logged BEFORE this
				 * checkpoint is already on the trusted base -- skip
				 * its logical redo.  The checkpoint's own frame LSN
				 * is the boundary in the (possibly re-numbered) log;
				 * records at or after it are redone in place.  A
				 * full-compaction checkpoint carries start_lsn == 0
				 * and imposes no skip (the following dump IS the
				 * whole live set).
				 */
				if (start != 0 && lsn > r->redo_start)
					r->redo_start = lsn;
			}
		} else if (h.type == XL_UPDATE) {
			xl_hdr_t uh;
			xl_body_t b;
			/* Below the fuzzy-checkpoint boundary the change is already
			 * on the trusted base -- skip its logical redo (and its
			 * loser tracking: a pre-checkpoint txn is resolved). */
			if (r->redo_start && lsn < r->redo_start) {
				off += (uint32_t)rl;
				continue;
			}
			if (xl_parse_update(base + off, (uint32_t)rl, &uh, &b) == XTC_OK) {
				uint8_t key[XS_VKLEN];
				uint8_t buf[1 + XS_VMAX];
				uint16_t vl = b.redo_len > XS_VMAX ? XS_VMAX : b.redo_len;
				struct rec_txn *t;
				enc_vkey(b.tableid, b.rowid, b.commit_ts, key);
				buf[0] = b.flags;
				if (vl) memcpy(buf + 1, b.redo, vl);
				/* An update that cannot be applied (a page read
				 * failed, no frame, no memory) stops the
				 * replay: a tree missing a committed row must
				 * be neither served nor compacted over the log
				 * that still holds it. */
				rc = bt_insert(r->bt, key, XS_VKLEN, buf,
				    (uint16_t)(1 + vl));
				if (rc != XTC_OK) {
					r->rc = rc;
					return 1;
				}
				if ((t = rec_txn_get(r, uh.txn_id)) == NULL ||
				    rec_txn_addupd(t, lsn, b.tableid, b.rowid,
				        b.commit_ts) != XTC_OK) {
					r->rc = XTC_E_NOMEM;
					return 1;
				}
				if (b.commit_ts > r->max_ts) r->max_ts = b.commit_ts;
			}
		} else if (h.type == XL_COMMIT) {
			int i;
			for (i = 0; i < r->n_txn; i++)
				if (r->txn[i].txn_id == h.txn_id) {
					rec_txn_retire(r, &r->txn[i]);  /* winner: redo stays */
					break;
				}
		}
		/* XL_BEGIN / XL_ABORT / XL_CLR / XL_END: no redo action.
		 * XL_PAGE was applied in pass 1 (in-place) or is a no-op
		 * (rebuild mode, r->bm == NULL). */
		off += (uint32_t)rl;
	}
	r->records++;
	return 0;
}

int
xstore_recover(bt_t *bt, const char *wal_path)
{
	return xstore_recover_ex(bt, wal_path, NULL);
}

int
xstore_recover_ex(bt_t *bt, const char *wal_path, struct wal_tail *tail)
{
	struct xs_recover r;
	wal_tail_t t;
	int rc, i;

	xs_commit_state_reset();
	memset(&r, 0, sizeof r);
	r.bt = bt;
	rc = wal_scan_ex(wal_path, xs_recover_cb, &r, &t);
	if (rc == XTC_OK)
		rc = r.rc;

	/* Whatever transactions are still active are losers: undo them,
	 * writing CLRs through an append handle on the same log (opened at
	 * the tail the scan found, no second scan).  Skipped entirely in the
	 * NO-STEAL path (no losers), and after a replay that stopped short:
	 * it has not seen every commit, and it writes nothing to the log. */
	if (rc == XTC_OK && r.n_txn > 0) {
		wal_t *w = NULL;
		wal_opts_t wo;
		memset(&wo, 0, sizeof wo);
		wo.path = wal_path; wo.append = 1;
		wo.tail = &t;
		if ((rc = wal_open(&wo, &w)) == XTC_OK) {
			for (i = 0; i < r.n_txn && rc == XTC_OK; i++)
				rc = xs_undo_loser(bt, w, &r.txn[i]);
			if (wal_get_tail(w, &t) != XTC_OK)
				t.valid = 0;           /* the CLRs moved the end */
			wal_close(w);
		}
	}
	if (rc != XTC_OK)
		t.valid = 0;
	if (tail != NULL)
		*tail = t;
	for (i = 0; i < r.n_txn; i++)
		free(r.txn[i].upd);
	free(r.txn);

	xstore_set_clock(r.max_ts + 1);   /* clock and read horizon */
	return rc;
}

/*
 * In-place crash recovery (ARIES physiological redo).  Unlike
 * xstore_recover -- which rebuilds the whole tree by replaying the
 * LOGICAL update log onto a FRESH (truncated) page file -- this trusts
 * a possibly-torn base in place.  It runs in two forward passes over the
 * (checksum-verified) log:
 *
 *   Pass 1 (redo images): apply every XL_PAGE full-page after-image the
 *   engine logged -- split/merge pages (SMO) AND plain non-split leaves
 *   (xs_leaf_page) -- page-LSN gated (bm_apply_page_image), so every
 *   torn page (a partly-flushed SMO, or a torn NON-split leaf) is
 *   repaired from its newest image BEFORE any logical redo descends it.
 *   Applying images in LSN order converges to each page's final bytes.
 *
 *   Pass 2 (logical redo + undo): replay the logical XL_UPDATE upserts
 *   over the now-repaired base (idempotent: version keys are immutable),
 *   retiring committed winners, then undo the losers exactly as in
 *   xstore_recover.  Because pass 1 already restored every torn leaf,
 *   the logical redo never has to navigate torn structure -- which is
 *   what made the earlier single-pass logical redo lose a torn
 *   non-split leaf's whole key range (the design notes S3).
 *
 * Requires `bm` to be the same buffer manager `bt` runs on, opened
 * reopen=1 (not truncated) with lsn_off >= 0, AND the engine's
 * physiological logging installed for the build (xstore_register_smo(1),
 * which now also logs non-split leaf images).  `out_pages` (optional)
 * receives the count of page images applied in pass 1 -- nonzero proves
 * the physiological path repaired something rather than rebuilding.
 */
int
xstore_recover_inplace(bt_t *bt, bm_t *bm, const char *wal_path,
    uint64_t *out_pages)
{
	struct xs_recover r;
	int rc, i;

	xs_commit_state_reset();
	memset(&r, 0, sizeof r);
	r.bt = bt;
	r.bm = bm;               /* in-place mode: apply XL_PAGE images */

	/* Pass 1: apply physiological page images (page-LSN gated) so torn
	 * pages are repaired before pass 2's logical redo descends them. */
	r.page_pass = 1;
	rc = wal_scan(wal_path, xs_recover_cb, &r);
	if (rc == XTC_OK)
		rc = r.rc;
	if (out_pages != NULL)
		*out_pages = r.pages_redone;
	if (rc != XTC_OK)
		return rc;

	/* Pass 2: logical redo + winner retire over the repaired base. */
	r.page_pass = 0;
	rc = wal_scan(wal_path, xs_recover_cb, &r);
	if (rc == XTC_OK)
		rc = r.rc;

	if (rc == XTC_OK && r.n_txn > 0) {
		wal_t *w = NULL;
		wal_opts_t wo;
		memset(&wo, 0, sizeof wo);
		wo.path = wal_path; wo.append = 1;
		if ((rc = wal_open(&wo, &w)) == XTC_OK) {
			for (i = 0; i < r.n_txn && rc == XTC_OK; i++)
				rc = xs_undo_loser(bt, w, &r.txn[i]);
			wal_close(w);
		}
	}
	for (i = 0; i < r.n_txn; i++)
		free(r.txn[i].upd);
	free(r.txn);

	xstore_set_clock(r.max_ts + 1);   /* clock and read horizon */
	return rc;
}

/* Dump the LIVE database state as WAL records, in the same payload
 * format wal_scan delivers to xs_recover_cb -- the body of an in-WAL
 * checkpoint.  Emits an XL_CHECKPOINT first (carrying the persisted
 * commit clock), then walks the whole B-tree (table-id 0's catalog rows
 * included) and for each (tableid, rowid) emits only the NEWEST version
 * (versions sort newest-first within a key) as a one-version
 * transaction frame [XL_UPDATE][XL_COMMIT], skipping tombstones and all
 * superseded versions, and every row of a dropped table (its catalog
 * row's newest version is a tombstone).  This is the compaction that
 * bounds the log:
 * regardless of how much churn preceded it, the dump -- and the log it
 * replaces -- is proportional to the live row count.  Safe only at a
 * quiesced checkpoint.  Original commit timestamps are preserved. */
/* snap != 0: dump each row as of that snapshot (the newest version
 * with ts <= snap), for an online checkpoint; 0 = the newest version. */
struct xs_dump_ctx { bt_t *bt; uint64_t clock; uint64_t snap; uint64_t n;
    int cancelled; };

static int
xs_dump_tree(wal_emit_fn emit, void *ectx, void *user)
{
	struct xs_dump_ctx *dc = user;
	bt_t *bt = dc->bt;
	bt_cursor_t *cur = NULL;
	uint8_t startk[XS_VKLEN];
	uint8_t ck[XL_HDR_LEN + 16];
	uint32_t last_tid = 0; int64_t last_rid = 0; int have_last = 0;
	int cn;
	struct xs_dropset dropped = { NULL, 0, 0 };
	uint32_t mark[2] = { 0, 0 };   /* high-water rows: table, index catalog */

	if ((cn = xl_enc_checkpoint(ck, sizeof ck, dc->clock, 0)) > 0)
		emit(ectx, ck, (uint32_t)cn);

	enc_vkey(0, INT64_MIN, ~(uint64_t)0, startk);   /* before every key */
	if (bt_cursor_open(bt, startk, XS_VKLEN, &cur) != XTC_OK) {
		dc->cancelled = 1; /* nothing dumped: not swapped in */
		return XTC_E_IO;
	}
	for (;;) {
		const void *k = NULL, *vv = NULL;
		uint16_t klen = 0, vl = 0;
		const uint8_t *kb;
		uint8_t rec[64 + XS_VMAX];
		uint8_t *p = rec;
		xl_hdr_t h;
		xl_body_t b;
		uint64_t ts;
		uint32_t tableid;
		int64_t rowid;
		uint8_t flags;
		uint16_t payl;
		int n;

		if (dc->snap != 0 && (++dc->n & 1023) == 0) {
			/* Online dump: let writers at the leaf now and then, and
			 * stop at once if the engine is shutting down. */
			if (xtc_atomic_load_i32(&g_ock_cancel,
			        xtc_mo_acquire)) {
				dc->cancelled = 1;
				break;
			}
			if (bt_cursor_park(cur) != XTC_OK || bt_cursor_resume(cur) != XTC_OK) {
				dc->cancelled = 1;
				break;
			}
		}
		if ((n = bt_cursor_next(cur, &k, &klen, &vv, &vl)) != XTC_OK) {
			/* NOTFOUND is the end of the tree; anything else would
			 * leave a truncated image, which neither checkpoint
			 * swaps in (the quiesced one gets XTC_E_IO below). */
			if (n != XTC_E_NOTFOUND) {
				(void)xs_rd_count(n);
				dc->cancelled = 1;
			}
			break;
		}
		/* Secondary-index entries (XS_IDX_LEAD keys, not version keys)
		 * are derived data, not logged; skip them.  They sort between
		 * the user tables and the index catalog (table-id 0xFFFFFFFD), so
		 * stopping here -- as this loop used to -- dropped the index
		 * catalog from every compacted log.  Seek past the whole
		 * namespace rather than read it: after a trusted reopen the
		 * index pages are not in memory, and walking them made a TPC-C
		 * W=10 shutdown read ~1.5 GB of them (close 3.9 s -> 16 s). */
		if (klen >= 1 && ((const uint8_t *)k)[0] == XS_IDX_LEAD) {
			uint8_t past = XS_IDX_LEAD + 1;
			bt_cursor_close(cur);
			cur = NULL;
			if (bt_cursor_open(bt, &past, 1, &cur) != XTC_OK) {
				dc->cancelled = 1;    /* a truncated image: not swapped in */
				free(dropped.id);
				return XTC_E_IO;
			}
			continue;
		}
		if (klen != XS_VKLEN)
			continue;
		kb = (const uint8_t *)k;
		tableid = dec_tableid(kb); rowid = dec_rowid(kb);
		if (tableid == XS_STAGE_TID)
			continue;                  /* STEAL staging: never part of the live state */
		if (have_last && tableid == last_tid && rowid == last_rid)
			continue;                  /* older version of a key already dumped */
		ts = dec_ts(kb);
		if (dc->snap != 0 && ts > dc->snap)
			continue;                  /* committed after the snapshot */
		last_tid = tableid; last_rid = rowid; have_last = 1;
		flags = (vl >= 1) ? ((const uint8_t *)vv)[0] : 0;
		if ((tableid == XS_CAT_TABLEID || tableid == XS_IDXCAT_TABLEID) &&
		    rowid == 0)
			mark[tableid == XS_IDXCAT_TABLEID] = xs_hw_of(vv, vl);
		/* A dropped table (xs_dropset): its rows leave the log with
		 * its catalog row, so a rebuild does not bring them back.  A
		 * catalog tombstone above its catalog's mark is dumped as is
		 * (xs_hw_of), and so are its table's rows. */
		if ((tableid == XS_CAT_TABLEID || tableid == XS_IDXCAT_TABLEID) &&
		    rowid > 0 && (flags & XS_F_DELETED) &&
		    (uint32_t)rowid > mark[tableid == XS_IDXCAT_TABLEID])
			goto emit;
		if (tableid == XS_CAT_TABLEID && rowid > 0 &&
		    (flags & XS_F_DELETED))
			xs_dropset_add(&dropped, (uint32_t)rowid);
		if (flags & XS_F_DELETED)
			continue;                  /* newest is a tombstone: row is gone */
		if (tableid != XS_CAT_TABLEID && xs_dropset_has(&dropped, tableid))
			continue;
emit:
		payl = (vl >= 1) ? (uint16_t)(vl - 1) : 0;
		if (payl > XS_VMAX) payl = XS_VMAX;
		h.type = XL_UPDATE; h.txn_id = ts; h.prev_lsn = 0;
		memset(&b, 0, sizeof b);
		b.tableid = tableid; b.rowid = rowid; b.commit_ts = ts;
		b.flags = flags;
		b.redo = payl ? (const uint8_t *)vv + 1 : NULL; b.redo_len = payl;
		if ((n = xl_enc_update(p, sizeof rec, &h, &b)) < 0)
			continue;
		p += n;
		h.type = XL_COMMIT;
		if ((n = xl_enc_simple(p, sizeof rec - (size_t)(p - rec), &h)) < 0)
			continue;
		p += n;
		emit(ectx, rec, (uint32_t)(p - rec));
	}
	bt_cursor_close(cur);
	free(dropped.id);
	return dc->cancelled ? XTC_E_IO : XTC_OK;
}

int
xstore_checkpoint_wal(bt_t *bt, struct wal *w, const char *wal_path)
{
	struct xs_dump_ctx dc;
	memset(&dc, 0, sizeof dc);
	dc.bt = bt;
	dc.clock = xtc_atomic_load_u64(&g_xclock, xtc_mo_relaxed);
	return wal_checkpoint((wal_t *)w, wal_path, xs_dump_tree, &dc);
}

/*
 * Online log checkpoint.  xstore_checkpoint_wal rewrites the log with
 * commits quiesced for the whole dump -- 0.5-3.5 s at TPC-C W=1, ~16 s
 * at W=10, a third of the run with no commit.  This one quiesces only
 * to take the start point and to swap files:
 *
 *   1. quiesced: snap = the clock (nothing in flight, so every commit
 *      at or below it is applied and durable, and its log frame lies
 *      before the live log's durable end L0); pin snap against version
 *      GC; wal_ockpt_begin records L0.  Release.
 *   2. not quiesced: dump the tree AS OF snap, then copy the live log's
 *      frames after L0 (every commit after the release has ts > snap and
 *      a frame after L0).  Rounds of copying until the uncopied tail is
 *      small.  Version keys are immutable, so a version present in both
 *      the dump and the tail replays harmlessly.
 *   3. quiesced: copy the last frames, fsync, rename, rebind.
 *
 * Step 2 runs on a blocking-pool thread when called on a proc: the
 * checkpoint proc shares loop 0 with the WAL writer, and a dump on the
 * fiber would stall every commit's log ack anyway.  Under simulation the
 * pool runs inline (deterministic).  Returns XTC_OK, or an error with
 * the live log unchanged; *tail (optional) = live frames copied, by the
 * catch-up rounds and the quiesced finish together.
 */
static int    g_ock_slot = -1;           /* GC pin for the online dump */

/* DST pessimal point between the online checkpoint's phases: under the
 * simulator with buggify on, stall the checkpoint 1 ms (virtual) on a
 * seeded coin -- a slow dump or a slow disk -- so commits, and a
 * simulated crash, land between the dump, the catch-up rounds and the
 * quiesced finish.  A single relaxed load in production. */
static void
xs_ock_yield(const char *site)
{
	if (XTC_SIM_BUGGIFY(site) && xtc_sim_buggify_fault(500) &&
	    !xtc_pid_is_none(xtc_self()))
		(void)xtc_proc_sleep(1000LL * 1000);
}
struct xs_ock_job {
	struct xs_dump_ctx dc;
	wal_ock_t *k;
	int rc;
};

static int
xs_ock_body(void *arg)
{
	struct xs_ock_job *j = arg;
	wal_emit_fn fn;
	void *ctx;
	uint64_t left = 0;
	int round;
	wal_ockpt_emitter(j->k, &fn, &ctx);
	if (xs_dump_tree(fn, ctx, &j->dc) != XTC_OK) {
		j->rc = XTC_E_AGAIN;
		return 0;
	}
	for (round = 0; round < 64; round++) {
		xs_ock_yield("xstore.ockpt.catchup");
		if (xtc_atomic_load_i32(&g_ock_cancel, xtc_mo_acquire)) {
			j->rc = XTC_E_AGAIN;
			return 0;
		}
		if ((j->rc = wal_ockpt_catchup(j->k, &left)) != XTC_OK)
			return 0;
		if (left < (256u << 10))
			break;
	}
	return 0;
}

/* Test hook (xstore_checkpoint_online_hook); NULL in production. */
static void (*g_ock_hook)(void);

void
xstore_checkpoint_online_hook(void (*fn)(void))
{
	g_ock_hook = fn;
}

void
xstore_checkpoint_cancel(int on)
{
	xtc_atomic_store_i32(&g_ock_cancel, on ? 1 : 0, xtc_mo_release);
}

int
xstore_checkpoint_wal_online(bt_t *bt, struct wal *w, const char *wal_path,
    uint64_t *tail)
{
	struct xs_ock_job j;
	int rc, res = 0;

	if (bt == NULL || w == NULL || wal_path == NULL)
		return XTC_E_INVAL;
	memset(&j, 0, sizeof j);
	j.dc.bt = bt;
	if (!xs_quiesce_begin(XS_QB_CKPT))
		return XTC_E_AGAIN;
	j.dc.snap = xtc_atomic_load_u64(&g_xclock, xtc_mo_relaxed);
	if (j.dc.snap == 0)
		j.dc.snap = 1;
	j.dc.clock = j.dc.snap;
	snap_set(&g_ock_slot, j.dc.snap);
	rc = wal_ockpt_begin((wal_t *)w, wal_path, &j.k);
	xstore_quiesce_end();
	if (rc != XTC_OK) {
		snap_set(&g_ock_slot, 0);
		return rc;
	}
	xs_ock_yield("xstore.ockpt.dump");
	if (xtc_pid_is_none(xtc_self()) ||
	    xtc_blocking_run(xs_ock_body, &j, &res) != XTC_OK)
		(void)xs_ock_body(&j);
	snap_set(&g_ock_slot, 0);
	if (j.rc != XTC_OK) {
		wal_ockpt_abort(j.k);
		return j.rc;
	}
	if (g_ock_hook != NULL)
		g_ock_hook();
	xs_ock_yield("xstore.ockpt.finish");
	if (!xs_quiesce_begin(XS_QB_CKPT)) {
		wal_ockpt_abort(j.k);
		return XTC_E_AGAIN;
	}
	rc = wal_ockpt_finish(j.k, tail);       /* frees j.k */
	xstore_quiesce_end();
	return rc;
}

/*
 * Fuzzy-checkpoint dump: emit the CHECKPOINT record carrying the redo
 * horizon as its start-LSN (nonzero => "trust the base in place"),
 * then re-emit every existing WAL frame at or after the horizon.  The
 * pre-horizon records are dropped: their changes are already durable on
 * the flushed base, so recovery-in-place does not need them.  The
 * retained tail is re-numbered by the emit path; recovery replays it in
 * place over the trusted base.  This layer reads the OLD log at
 * fc->path (wal_checkpoint writes the compacted log to path.compact, so
 * the source is intact during the dump).
 */
struct xs_fuzzy_ctx {
	const char *path;
	uint64_t    clock;
	uint64_t    horizon;
	wal_emit_fn emit;
	void       *ectx;
};

static int
xs_fuzzy_tail_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	struct xs_fuzzy_ctx *fc = user;
	if (lsn >= fc->horizon)
		fc->emit(fc->ectx, rec, len);   /* retain: at/after the horizon */
	return 0;
}

static int
xs_dump_fuzzy(wal_emit_fn emit, void *ectx, void *user)
{
	struct xs_fuzzy_ctx *fc = user;
	uint8_t ck[XL_HDR_LEN + 16];
	int cn;

	fc->emit = emit;
	fc->ectx = ectx;
	/* start_lsn == horizon (nonzero): fuzzy checkpoint, base trusted. */
	if ((cn = xl_enc_checkpoint(ck, sizeof ck, fc->clock, fc->horizon)) > 0)
		emit(ectx, ck, (uint32_t)cn);
	return wal_scan(fc->path, xs_fuzzy_tail_cb, fc); /* NOMEM: cut short */
}

int
xstore_fuzzy_checkpoint(bt_t *bt, bm_t *bm, struct wal *w,
    const char *wal_path, uint64_t *out_horizon)
{
	struct xs_fuzzy_ctx fc;
	uint64_t horizon;
	int rc;

	(void)bt;
	if (bm == NULL || w == NULL || wal_path == NULL)
		return XTC_E_INVAL;

	/*
	 * recLSN horizon.  bm_min_rec_lsn(bm) is the oldest change not yet on
	 * the base -- the truncation floor a PURE (no-flush) fuzzy checkpoint
	 * would carry, pinned near the front by any long-lived dirty page
	 * (the root/interior nodes stay dirty across a whole build, so it
	 * barely advances on its own).  This checkpoint instead does the
	 * O(dirty) work of FLUSHING the whole dirty set (below), which makes
	 * every change through the current durable LSN redundant with the
	 * base and advances the safe truncation point to the post-flush
	 * durable LSN -- strictly at or above bm_min_rec_lsn, so it is sound
	 * and truncates far more.  That post-flush durable LSN is the horizon
	 * carried in the checkpoint record.
	 */

	/* Flush the dirty set to the base (O(dirty)) and fsync.  After this
	 * every change logged so far is on the base. */
	if ((rc = bm_checkpoint(bm)) != XTC_OK)
		return rc;

	/*
	 * The base is now durable through the current durable LSN, so every
	 * record at or below it is redundant -- truncate below it.  Retain
	 * only records strictly after it (concurrent commits; none at a
	 * quiesced checkpoint, so the retained tail is normally empty and the
	 * log becomes just the checkpoint record).  horizon > 0 marks this a
	 * fuzzy checkpoint so recovery trusts the base in place.
	 */
	horizon = wal_durable_lsn((wal_t *)w) + 1;

	/* Compact the log: keep only the tail at/after the horizon, led by a
	 * fuzzy CHECKPOINT record.  Atomic rename + rebind (wal_checkpoint). */
	fc.path = wal_path;
	fc.clock = xtc_atomic_load_u64(&g_xclock, xtc_mo_relaxed);
	fc.horizon = horizon;
	fc.emit = NULL; fc.ectx = NULL;
	if ((rc = wal_checkpoint((wal_t *)w, wal_path, xs_dump_fuzzy, &fc)) != XTC_OK)
		return rc;

	if (out_horizon != NULL)
		*out_horizon = horizon;
	return XTC_OK;
}
