# examples/05_rexis -- rexis (Redis-protocol-compatible) server built on xtc

A drop-in replacement for a meaningful subset of `redis-server`, implemented
in ~2,000 lines of C using xtc primitives.  Demonstrates how to build a
production-shaped network service with hard resource budgets.

## Quick start

```sh
# Build (assumes libxtc.a in ../../build_unix/)
cd examples/05_rexis
make

# Run with default settings (port 6379, no caps)
./rexis-server-xtc

# Run with hard caps for the P99-conference demo
./rexis-server-xtc \
    --port=16379 \
    --cores=2 \
    --max-memory=$((100*1024*1024)) \
    --max-keys=1000000 \
    --max-clients=10000 \
    --max-iops=100000

# Talk to it with the real redis-cli (or any RESP2/RESP3 client)
redis-cli -p 16379 PING
redis-cli -p 16379 SET hello world
redis-cli -p 16379 GET hello
```

## What's supported

35 commands across the major Redis data types:

| Category | Commands |
|---|---|
| Strings | GET, SET, DEL, EXISTS, INCR, DECR, INCRBY, DECRBY |
| Keys | EXPIRE, TTL, KEYS, FLUSHDB, DBSIZE |
| Lists | LPUSH, RPUSH, LPOP, RPOP, LLEN, LRANGE |
| Hashes | HSET, HGET, HDEL, HKEYS, HVALS, HGETALL, HLEN |
| Pub/Sub | SUBSCRIBE, UNSUBSCRIBE, PUBLISH |
| Connection | PING, ECHO, QUIT, AUTH, SELECT |
| Admin | INFO, COMMAND, CLUSTER NODES |

Both RESP2 (Redis 1.x-5.x) and RESP3 (Redis 6+) protocols are accepted,
as are inline commands (`PING\r\n`, `SET k v\r\n` -- what telnet and
`nc` send; words are split on spaces/tabs and not unquoted).

Pub/Sub is built on libxtc process groups (`xtc_pg`, over the
duplicate-key registry `xtc_reg_register_dup`): SUBSCRIBE joins the
connection's pid to a channel group, PUBLISH fans a RESP push frame out
to every subscriber's mailbox (which the connection proc appends to its
socket), and a closing connection calls `xtc_reg_drop_pid` to leave every
channel it joined.  See the pubsub walkthrough in the docs example page.

## What's NOT supported (deliberately)

Out of scope for a 2k-LOC example.  No Streams, no scripting
(EVAL/Lua), no cluster mode, no modules, no replication, no
Redis-compatible persistence formats (no AOF, no RDB -- rexis has its
own append-only bitcask store instead, see `--persist=DIR` below), no
transactions (MULTI/EXEC), no Sorted Sets (ZADD/ZRANGE).
A full Redis would add another 30k LOC.  Pub/Sub keyspace notifications
and pattern subscriptions (PSUBSCRIBE) are also out of scope.

## Resource budget enforcement

This is the centerpiece for the P99 conference demo.  The caps below
hold under stress:

| Flag | What it bounds | How |
|---|---|---|
| `--cores=N` | Which CPUs the process may run on | `sched_setaffinity(2)` restricts it to CPUs 0..N-1.  It pins; it does not add threads (that is `--loops`) |
| `--loops=N` | Event loops (OS threads) | Connections go to the least-loaded of N `xtc_exec` loops.  One loop holds as many clients as `--loop-clients` allows (up to 4,194,304; libxtc 2.0 has no per-loop proc limit, 1.x stopped at 65,536 procs, so 1.x rexis needed `--loops=2` for 100k clients).  With N > 1 the keyspace is shared by N threads and every command takes a parking reader/writer latch (`xtc_arwlock`); with 1 it takes none |
| `--max-memory=N` | Keyspace + client buffers | One `xtc_res` `XTC_RES_MEM_BYTES` budget charged by every keyspace allocation (including the bucket array) AND by every client query/output buffer's growth above a fixed 16 KiB each.  A write that would pass it gets `-OOM`; a client whose buffer would pass it gets `-OOM` and is closed.  `INFO` `used_memory` is the budget's total (`used_memory_dataset` + `used_memory_clients`).  Not covered: the first 16 KiB of each of a connection's two buffers (bounded by `--max-clients`; allocated on first use), proc stacks, allocator overhead.  0 = no cap |
| `--max-keys=N` | DB key count | Per-DB counter; SET/HSET/LPUSH refused above cap |
| `--max-clients=N` | Concurrent connections (default 10000; 0 = all the loops hold) | Listener checks the live count before spawning a per-conn proc.  An over-limit client gets `-ERR max number of clients reached` (Redis's reply) and a close, as does one that finds every loop full (`--loop-clients=N` lowers the per-loop cap; INFO shows `connected_clients`, `rejected_connections`, `loopN_clients`).  A cap above `--loops` x 4,194,304 is refused at startup with the `--loops` value that fits |
| `--max-fds=N` | Sockets, listener included (default: `--max-clients` + 64) | The `xtc_res` `XTC_RES_FDS` budget, attached with `xtc_res_attach_net`, so `xtc_net_accept` refuses BEFORE accepting: a client past it waits in the kernel backlog and is served when a socket closes.  The soft `RLIMIT_NOFILE` is raised to fit |
| `--max-iops=N` | Commands/sec | Token bucket refilled every second; excess commands are DELAYED (held in the connection's buffer until tokens return), not rejected |
| `--max-request=N` | Largest command (default 64 MiB) | Redis `proto-max-bulk-len`: a bulk whose header announces more is refused at once (`-ERR Protocol error: ...`) and the connection closed; the query buffer grows only to hold one command |
| `--max-output=N` | Unsent reply bytes per client (default 128 MiB) | Redis `client-output-buffer-limit`: a reply that would pass it is replaced WHOLE by `-ERR reply exceeds the client output limit` (never truncated); a pub/sub subscriber that falls that far behind is disconnected |
| `--timeout=SECS` | Idle connections (default 0 = never) | Redis `timeout`: no byte in either direction for SECS closes the connection; subscribers are exempt |

Zero-copy GET: a string value of 4 KiB or more is stored behind a
reference count, and GET sends it straight from the keyspace with
`sendmsg` (the reply header and CRLF come from the output buffer)
instead of copying it into the output buffer.  The reply holds a
reference until its last byte has left, so a SET, DEL or expiry that
replaces the value while a partial send has the fiber parked frees
nothing that is still being sent: the old value is freed by whichever
of the two finishes last.  The bytes held this way count toward
`--max-output`, the 1 MiB backpressure mark and the `--max-memory`
client budget exactly as a copy would.  Smaller values are still
copied; the copy is cheaper than the reference.  Measured (one loop,
memtier GET-only, 32 connections, pipeline 8, 3 interleaved runs):
64 KiB GETs 55.7k -> 75.2k per second (+35%; the copy was 28% of the
server's CPU samples, now 0.1%), 1 KiB unchanged within noise (592.6k ->
635.4k).  `INFO` `reply_bytes_by_ref` counts the value bytes sent this
way.  `xtc_rcu` was considered for the lifetime and not used: a slow
client can keep a reply unsent for seconds, and an RCU read section held
across that park would delay every reclamation in the process.

Backpressure: once more than 1 MiB of a client's replies are unsent, its
connection stops reading and running commands until the client drains
them, so a client that pipelines without reading is slowed by TCP flow
control rather than growing the server.  Malformed input is answered
with an error and the connection is closed (the rest of the stream
cannot be trusted).  Under fd exhaustion (EMFILE) the listener backs off
10 ms per failed accept instead of spinning, and recovers when fds free.

Verified by MEASUREMENT in `test/m99/test_rexis_budgets.c`: server RSS
growth while offered 4x the memory cap and while 100 clients each hold a
990 KB partial command, the server's CPU affinity mask, connections
actually served, the reply rate of a pipelined burst (<= 1.5x
`--max-iops`), server CPU while a client holds an oversized partial
bulk and under an fd flood, and the idle timeout.  Run with
`make check-rexis` in a build dir.

## Architecture

```
+------------------------+
|  xtc_app               |  startup / shutdown / supervisor
|     |                  |
|  xtc_supervisor (one_for_one)
|     |
|     +-- listener_proc          accepts; spawns per-conn procs
|     |
|     +-- expire_proc            timer-driven TTL expiry
|     |
|     +-- metrics_proc           periodic xtc_log of res stats
|     |
|     +-- N x conn_proc          one xtc_proc per accepted client;
|         (BEAM-style)           reads RESP cmds, dispatches, writes resp
|
+------------------------+
        |
        v
+------------------------+
| xtc_lrlock-protected   |  Linear-hash table (FNV-1a, lhash.h) keyed by
|       database         |  string; grows one bucket per insert; values
|                        |  are tagged unions (string/list/hash)
+------------------------+
```

xtc primitives used:

* **xtc_loop / xtc_io_poll** -- event loop and TCP accept/read/write
* **xtc_net_listen** -- listening socket with the standard knobs
* **xtc_proc** -- one BEAM-style process per accepted connection
* **xtc_lrlock** -- wait-free reads on the database
* **xtc_res** -- the resource accountant; the centerpiece
* **xtc_log** -- structured operational logging
* **xtc_cfg** -- runtime configuration knobs
* **xtc_app + xtc_supervisor** -- clean startup/shutdown
* **xtc_inject** -- fault-injection hooks (unused in steady state, present for tests)

## Files

| File | Lines | Purpose |
|---|---|---|
| `main.c` | 486 | Entry point, arg parsing, app bringup |
| `proto.c` | 387 | RESP2/RESP3 parser + response builder |
| `proto.h` | 149 | Protocol type definitions |
| `db.c` | 1,183 | Database (linear-hash table + lrlock) |
| `lhash.h` | 166 | Linear hashing: the growing table under db.c and the bitcask index |
| `db.h` | 209 | Database API |
| `cmd.c` | 741 | Command dispatch + 35 handlers |
| `cmd.h` | 64 | Command type definitions |
| `conn.c` | 296 | Per-connection xtc_proc |
| `conn.h` | 48 | Connection API |
| `expire.c` | 64 | TTL expiration timer |
| `metrics.c` | 77 | Periodic res-stats logging |
| `Makefile` | 60 | Build glue |

Total: ~3,600 LOC of source.

## Tests

In `test/m99/`:

| Test | Cases | Coverage |
|---|---|---|
| `test_resp_parser.c` | 39 | RESP2/RESP3 framing, inline commands, malformed input, integer overflow |
| `test_rexis_loopback.c` | 17 | End-to-end round-trips, replies larger than any buffer, slow readers, uncapped LRANGE/KEYS/HGETALL |
| `test_rexis_budgets.c` | 11 | Every `--max-*` cap, measured; request/output limits, client-buffer memory, idle timeout, EMFILE; clean SIGTERM |
| `test_rexis_pbt.c` | 5 | properties (atomicity, idempotence, FIFO) |

Run via `make check-rexis` in a libxtc build dir (CI runs it in the
examples job).  Loopback, budget and PBT tests spawn a real
`rexis-server-xtc` on a kernel-chosen free port (see
`test/m99/rexis_harness.h`); every test also requires the server to exit
0 on SIGTERM.

## Bench

In `bench/rexis_compat/`:

```sh
cd bench/rexis_compat
make
./bench --host=127.0.0.1 --port=16379 --pipeline=1 --requests=100000
```

Reports ops/sec and p50/p99/p999 in the M17 conformance format.  When
real `redis-server` is on `$PATH`, a side-by-side comparison can be run.

## Gaps in xtc surfaced by this example

Things this example needed that xtc doesn't provide cleanly today.
Each was an opportunity for a new xtc primitive.  **Four of the eight
originally listed here have since shipped** -- the entries are kept, and
marked, because the list doubles as a record of how consumer pressure
shaped the library:

1. ~~**No built-in hash map / dict primitive.**~~  **SHIPPED.**  Both
   `xtc_chash` (concurrent, RCU-protected, `<xtc_chash.h>`) and
   `xtc_pdict` (per-proc dictionary, `<xtc_pdict.h>`) exist now.  rexis
   still uses its own table (`lhash.h`, linear hashing over intrusive
   chains): one loop owns the keyspace, so it needs no locking or RCU,
   and it grows one bucket at a time with no whole-table rehash.  Until
   2026-10 it was a FIXED 65,536-bucket array that never resized -- at 1M
   keys that cost 40% of its pipelined throughput, at 5M a 4.4x slower
   load.  A consumer that shares a table across loops should reach for
   `xtc_chash`.

2. **No token-bucket rate limiter.**  Still true.  The IOPS cap is
   implemented inline with atomics (~30 LOC).  `xtc_credit`
   (`<xtc_credit.h>`) covers the related *windowed* flow-control case --
   see `examples/07_kaka/broker.c` -- but not a time-based token bucket.

3. **No glob pattern matcher.**  Still true.  `KEYS pattern` needed one;
   ~40 LOC inline.

4. **No `xtc_proc` enumeration.**  Partly addressed: `xtc_inspect`
   (`<xtc_inspect.h>`) can enumerate live procs for observability.  rexis
   still tracks `conn_count` with a manual atomic.

5. **No core-pinning API in `xtc_exec`.**  Still true for pinning
   itself, but the *topology* half now ships: `xtc_ncpus()`,
   `xtc_numa_nnodes()`, `xtc_numa_node_of_cpu()`,
   `xtc_numa_current_node()`.

6. ~~**No non-blocking `xtc_recv` variant.**~~  **SHIPPED (it always
   existed -- this entry was wrong).**  `xtc_recv(&m, &sz, 0)` returns
   immediately; a `0` timeout is documented as non-blocking in
   `<xtc_proc.h>`.  rexis itself uses it in `conn.c`.

7. ~~**No timer inside `xtc_proc`.**~~  **SHIPPED.**  `xtc_proc_sleep()`
   parks the fiber (without blocking the OS thread) for a bounded time;
   a recv timeout also remains a fine way to do periodic work.

8. ~~**No async-fd readiness in `xtc_proc`** -- once "the biggest gap".~~
   **SHIPPED, and rexis now uses it.**  `xtc_proc_wait_fd(fd, interest,
   timeout_ns, &revents)` is exactly the primitive this entry asked for
   by name, and the listener + per-connection procs park on it
   (`conn.c`, `main.c`) instead of busy-polling.  The benchmark numbers
   below predate that change and should be re-measured.

## Known limitations

* Plaintext RESP only.  No TLS yet (TLS-Redis is a separate exercise;
  see libxtc's `docs/tls.md` and the existing `xtc_tls` API for the building
  blocks).
* Persistence is a log-structured **bitcask** store (`bitcask.c`), not
  Redis's AOF/RDB formats: run with `--persist=DIR` and the keyspace is
  durable across restart (`db.c` replays the log at startup; covered by
  `test/m99/test_rexis_persist.sh`).  Without `--persist` rexis is
  RAM-only.  Note the store's file I/O is currently synchronous -- see
  the note in `bitcask.h` about why, and `examples/06_sqlxtc/wal.c` for
  the `xtc_aio_*` alternative.
* `KEYS pattern` is O(n) like real Redis -- documented, not a bug.
* `--loops=N` spreads connections, and with N > 1 the keyspace is cut
  into 64 shards by key hash.  The lrlock guards only a table header;
  both copies share the buckets and entries, and writers free entries
  in place, so each shard also has an `xtc_arwlock` (readers share it,
  writers to that shard serialize).  A command takes only the shards of
  its keys, in ascending order (`db_lock`; DEL and EXISTS may name
  several); KEYS, FLUSHDB, expiry and the persistence replay take all
  of them.  SET/s with valkey-benchmark -c 200 --threads 8 -P 16
  -t set -d 32 -r 1000000, median of 3 interleaved runs [range]:

  | `--loops` | 1 | 2 | 4 | 8 | 16 | 32 |
  |---|---:|---:|---:|---:|---:|---:|
  | SET/s | 0.78M [0.78-0.79] | 1.14M [1.14-1.14] | 1.75M [1.70-1.78] | 2.53M [2.48-2.53] | 3.20M [3.06-3.25] | 3.50M [3.33-3.61] |
  | server cores busy | 1.0 | 1.9 | 3.4 | 6.0 | 10.3 | 16.7 |

  Measured on libxtc 2.1.1-37 (`a2134a08`, examples `7748ab9`) on a
  two-socket 64-core x86-64 bare-metal host (Ice Lake, 128 threads),
  Debian 13, the server on NUMA node 0 (its first N cores, then their
  SMT siblings) and the client on node 1.  A client four times as wide
  (--threads 32 -c 512) is no faster (2.40M at 8 loops, 2.93M at 16,
  3.36M at 32), so the server, not the client, bounds it.  Every SET
  write-latches its key's shard, and past 16 loops each extra loop adds
  less than a tenth of a loop's worth: 16 more loops buy 9%.  The same
  client against DragonFly (`--proactor_threads N`): 0.38M, 0.72M,
  1.36M, 2.57M, 4.33M, 4.30M; against Valkey (`--io-threads N`) at most
  0.87M (at 2).  rexis leads to 4, they are level at 8, and DragonFly
  leads at 16 and 32.  On a 32-vCPU host, one latch
  for the whole keyspace gave 0.23M at `--loops=2` and 0.14M at
  `--loops=4` (one loop: 0.89M).  The shards are not
  a DragonFly design: any loop runs any command on any shard, so a hot
  key still serializes its writers and its cache lines move between
  cores -- see `docs/examples/rexis-vs-dragonfly.md`.
* An idle connection that has sent nothing costs ~7.3 KB of RSS (its
  fiber stack and per-connection state; Valkey: 0.9 KB): the query and
  output buffers are allocated by the first read and the first reply
  (20,000 idle connections on `--loops=2`: 15.7 KB each when both
  16 KiB buffers were allocated at accept).  It does not wake: with no
  `--timeout` and nothing buffered its proc parks on the socket with no
  deadline, and shutdown wakes it explicitly (an fd -> pid table in
  `conn.c`).  Each idle proc used to wake once a second to look at the
  shutdown flag; with 100,000 idle connections on `--loops=2` that cost
  526 CPU ticks per 10 s with no traffic and held memtier to 134k ops/s.
  Now: 8 ticks (the same as with no connections) and 179k ops/s (medians
  of 3 interleaved runs).
* A client that pipelines an unbounded stream and never reads stalls
  once the socket buffers fill (backpressure, above); Redis instead
  buffers replies without limit for normal clients by default.
* `INFO` output is hand-rolled and approximate; real Redis has
  hundreds of metrics; we expose ~30.
* `CLUSTER NODES` returns a single-node response unconditionally.

## Comparison vs Valkey and DragonFly

Measured 2026-11 on libxtc 2.1.1-37 (`a2134a08`, examples `7748ab9`), a
two-socket 64-core x86-64 bare-metal host (Ice Lake, 128 threads),
Debian 13, loopback, memtier_benchmark GET:SET 10:1, 32 B values, 1M
keys prefilled, the server on NUMA node 0 and the client on node 1,
medians of 3 interleaved runs (the method, every thread count, the
open-loop tails and the persistence table are in
`docs/examples/rexis-vs-dragonfly.md`):

| | Valkey 9.0.6 | DragonFly | rexis-server-xtc |
|---|---:|---:|---:|
| ops/s, 1 thread, no pipelining | 101,000 | 92,000 | 103,000 |
| ops/s, 1 thread, pipeline 16 | 752,000 | 442,000 | 848,000 |
| ops/s, 1 thread, client on the server's node | 169,000 | 147,000 | 174,000 |
| ops/s, 32 threads (`--loops=32`), no pipelining | 645,000 | 2,305,000 | 2,387,000 |
| ops/s, 32 threads, pipeline 16 | 1,064,000 | 9,093,000 | 11,274,000 |
| open-loop p99 at 50k ops/s, 1 thread | 32 us | 32 us | 30 us |
| load 5M keys (pipelined SET; memtier reports whole seconds) | 8 s | 11 s | 6 s |

The old gap at one thread without pipelining is gone: rexis makes 2.1
system calls per request (recvfrom + sendto, and one `io_uring_enter`
per 8 requests; strace -c over 5 s at one thread), Valkey 2.0, DragonFly
2.0, where 1.59 rexis made about 7.  fcontext is the default fiber
switch on glibc x86-64 now, so a switch makes no `rt_sigprocmask` call,
and the io_uring backend submits a park's poll in the same
`io_uring_enter` that waits.  Polls are still one-shot: a
socket whose poll fired is re-armed at its next park (DragonFly keeps one
multishot poll per socket).
With `--loops=N`, rexis scales on this read-heavy load as far as
DragonFly does. That is a 64-shard latched keyspace, not DragonFly's
shard per thread, so a hot key or a write-heavy load still serializes
on a latch (the rexis-vs-dragonfly page shows where its tail rises
first).

We are NOT claiming faster than real Redis here; the value is in the
**bounded behaviour under stress** (all 5 caps hold; real Redis lets you
OOM the host on `maxmemory` overrun via fragmentation).
