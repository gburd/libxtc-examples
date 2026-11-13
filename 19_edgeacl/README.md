# 19_edgeacl -- an edge gateway's IP access table: ordered map, RCU, per-session settings

`edgeacl.c` is the address check at the front of an edge gateway.  Eight
client sessions, spread over the four loops of an `xtc_exec`, look up each
client's address in a table of address ranges: which allocation it falls
in, which country that is, and whether the range is allowed or denied.  A
feed process rewrites the table while they read it.  Two settings decide
what the gateway does with the answer, and each session can override them.

```sh
make 19_edgeacl && ./19_edgeacl            # from a libxtc build tree
./19_edgeacl -v                            # also: lookups per session
make XTC_BUILD=../../build_unix test       # from this directory
```

## The table: an ordered map read while it changes

The table is a set of disjoint ranges `[start, end]`, keyed by `start`.
"Which range holds this address" is the range with the largest start at
or below the address, if its end reaches the address.  That is a floor
lookup, `xtc_cskip_floor`, and it is the reason the table is an
`xtc_cskip` (a skiplist) and not an `xtc_chash`: a hash table can only
answer "is this exact key present".

The feed applies 46 updates while the sessions look up random addresses:
it reassigns ranges to new owners (an insert that replaces the value),
allocates gaps (inserts), withdraws ranges (removes), and splits a /16
into two /17s (a remove and two inserts).

A lookup is an RCU read-side around the floor lookup, with the answer
copied out before the read-side ends:

```c
xtc_rcu_read_lock();
if (xtc_cskip_floor(table, &ip, NULL, &val) == XTC_OK) {
	const struct range *r = val;
	if (ip <= r->end)
		answer = *r;      /* copy out: r is not valid after unlock */
}
xtc_rcu_read_unlock();
```

The writer never frees anything a reader might hold.  A replaced record
and a removed range's key and record go to `xtc_rcu_retire`, and the feed
calls `xtc_rcu_synchronize` every eight updates, which frees them once
every reader that could still see them has left its read-side.

**How it is checked.**  The program keeps a snapshot of the table after
every feed version.  The feed publishes a version number before and after
each update.  A lookup records the version finished before it began and
the version started before it ended, and its answer must be the table's
answer at some version in between.  The feed waits, after each update,
until every session has made 64 more lookups, so every version is read
from every loop however the OS schedules the loop threads.  A run makes
about 380,000 lookups during the feed, and the program requires every one
of them to match.

## The settings: global, per session, and per request

Two settings are registered in `xtc_cfg`:

| name | values | meaning |
|---|---|---|
| `edge.mode` | `enforce`, `audit` | deny, or log the denial and let it through |
| `edge.unknown` | `allow`, `deny` | the verdict for an address no range holds |

The hot path reads them through `xtc_cfg_ref` handles, resolved once, so
a read does no name lookup.  Each session creates an `xtc_cfg_session` and
binds it to its fiber, and a read resolves "this session's override, else
the global value":

* The two **partner** sessions set `edge.mode=audit` from their
  connection's startup options (source `XTC_CFG_SRC_CLIENT`).  The public
  sessions have no override.
* Halfway through, the **operator** changes the global `edge.unknown` from
  `allow` to `deny`.  Every session without an override sees it on its
  next read, on every loop; the `on_change` callback fires once.
* A **dry-run** request pushes a level, sets `edge.mode=audit` for that
  request, and aborts the level afterwards (`SET LOCAL`), so the next
  request is enforced again.
* A per-role default (`XTC_CFG_SRC_USER`) that tries to set
  `edge.mode=enforce` on a partner session is ignored: it ranks below the
  client's startup option.

After the feed, the table and the settings are frozen between barriers,
and every session answers the same eight scripted requests, before and
after the operator's change:

```
batch 2:
  10.2.17.9     in no range               public: deny   partner: audit
  10.0.4.4      in 10.0.0.0/16 JP deny    public: deny   partner: audit
  10.1.200.1    in 10.1.128.0/17 BR allow public: allow  partner: allow
  ...
```

Each session's answer is checked against the frozen table, and its verdict
against its own settings.  `expected.txt` is the full default output; the
lookup counts vary from run to run and are printed only with `-v`.

## Design choices

* **Keys and records in their own allocations.**  The map stores key and
  value pointers verbatim.  On a replace it keeps the key it already has
  and swaps only the value, so the program frees the new key it passed
  in (never published) and retires the old value (a reader may hold it).
  `xtc_cskip_remove` does not return the key, so `table_del` looks the key
  up first and retires it with the value.
* **Copy the answer out of the read-side.**  The range record is valid
  only until `xtc_rcu_read_unlock`.  The answer struct holds the country
  code as a pointer into a static table, which stays valid.
* **The feed reclaims.**  `xtc_cskip` never calls `xtc_rcu_synchronize`
  itself.  The writer, which knows how much it retired, calls it every
  eight updates.  `xtc_rcu_fini` at exit runs what is still pending.
* **Barriers for the frozen phase.**  `xtc_barrier_wait` parks a fiber
  without blocking its loop, so the eight sessions and the feed line up
  across four loops.
* **Snapshots instead of locks in the test.**  The check needs no lock
  around the table and does not slow the readers down: the snapshots are
  computed before the run and only read during it.

## Alternatives not chosen

* **A reader-writer lock around a sorted array.**  Simple, and a binary
  search is fast.  Every update then rebuilds or shifts the array under
  the write lock while every lookup waits.  A gateway takes lookups on
  every connection and updates from a feed, so readers should not wait on
  the writer.
* **Copy-on-write of the whole table** (build a new array, swap one
  pointer, retire the old).  Readers are as cheap as it gets, and it is a
  good fit for a table that changes a few times a day.  This feed changes
  ranges one at a time, and copying 40 ranges per update is cheap here but
  not for a full routing table of a million.
* **An interval tree.**  Needed when ranges overlap.  Allocations do not
  overlap, so the floor lookup plus an end check answers the question with
  a plain ordered map.
* **Settings as globals read with atomics.**  Fast, and enough for one
  global value.  It cannot give two sessions different values, or roll a
  value back at the end of a request; the session layer exists for that.

## Cost, measured

On a 48-core x86-64 host the whole run takes about 0.1 s.  Note that in
libxtc 2.2.0 the read-side is cheaper off a fiber than on one:
`xtc_rcu_read_lock` and `_read_unlock` on a fiber take a short internal
mutex to find the fiber's reader slot, so fiber readers on many loops
contend on it (measured at about 7 ns per empty bracket on eight OS
threads and about 1.9 us on eight fibers on eight loops).  The example's
correctness does not depend on that cost; its throughput does.
