/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/sx_dst.h
 *	The engine's planted bugs, for libxtc's DST bug-detection gate
 *	(scripts/dst-bug-inject.sh in libxtc).  Each id guards one
 *	deliberate durability or safety defect behind XTC_DST_BUG(n)
 *	(xtc_sim.h), which is 1 only in a build made with
 *	-DXTC_DST_INJECT_BUG=n, so no normal build carries any of them.
 *	The ids are fixed by the gate's case table; they share libxtc's
 *	numbering only because the gate drives both with one flag.
 */

#ifndef SQLXTC_SX_DST_H
#define SQLXTC_SX_DST_H

#include "xtc_sim.h"

#define XTC_DST_BUG_NODURABLE  3   /* wal.c: skip fdatasync, still ack */
#define XTC_DST_BUG_WALKILL    17  /* wal.c: commit not masked against kills */
#define XTC_DST_BUG_IDXSKIP    18  /* xstore.c: an index rebuild skips one index */
#define XTC_DST_BUG_EVICTNOWB  19  /* bufmgr.c: evict a dirty page unwritten */
#define XTC_DST_BUG_OCKDROP    20  /* wal.c: a checkpoint copy round drops its last frame */
#define XTC_DST_BUG_PRUNEHZN   21  /* xstore.c: prune-on-write deletes the version at the horizon */
#define XTC_DST_BUG_SHIFTLO    22  /* btree.c: a resumed cursor skips the lo-fence re-check */
#define XTC_DST_BUG_GCBATCH    23  /* xstore.c: version GC forgets its row at a batch boundary */

#endif /* SQLXTC_SX_DST_H */
