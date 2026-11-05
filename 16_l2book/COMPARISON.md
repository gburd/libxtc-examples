# l2book against intrepidkarthi/orderbook

l2book was built to the feature set of
[intrepidkarthi/orderbook](https://github.com/intrepidkarthi/orderbook)
(at `3fffc25`, v0.30.0). This page compares the two at three levels, all
on one machine in one session:

1. the matching engine;
2. single operations;
3. the whole venue, over TCP.

At every level the comparison uses intrepidkarthi's own harness, or a port
of it that keeps every operation the same. Their engine is written to be
safe to run in production first, and to be fast second; the
[What the numbers do not say](#what-the-numbers-do-not-say) section says
what that costs it here.

**Host:** a bare-metal x86-64 server.

- **CPU:** two Ice Lake Xeon sockets (64 cores, 128 threads, 1 TiB), two
  NUMA nodes. Everything below runs on node 0 unless a section says
  otherwise.
- **OS:** Debian 13, kernel 6.12.
- **Tuning:** CPU governor set to `performance`, transparent huge pages
  off, idle states C1E and C6 disabled, NUMA balancing off, and nothing
  else running.
- **Compilers:** gcc 14.2, Go 1.24.4, Rust 1.99.1.
- **Code:** libxtc 2.1.1-37 (`a2134a08`) and examples `7748ab9`, plus the
  engine fix that makes the order-id map reclaim the slots of deleted
  orders (before it, a book refused every order once three quarters of
  the map had ever been used, live or not).

Each engine runs pinned to one core. An earlier run of the same
comparison on a one-socket 48-core Sapphire Rapids host, with the engine
before that fix, gave the same ranking at every level; its numbers were
lower across the board (l2book's replay 2.76 ms, theirs 9.60 ms).

## 1. The engine: one tape, one digest

intrepidkarthi's [cross-engine harness](https://github.com/intrepidkarthi/orderbook/blob/main/docs/CROSS-ENGINE.md)
replays a 50,000-command tape (`bench-basic-v1`: limit GTC submits and
cancels) through an adapter for each engine. It then reduces each
adapter's whole output to a single checksum, their OBDG "core digest",
built from:

- every command's accepted/refused flag;
- every trade;
- the final book.

All engines go through one encoder. An engine is timed only if its digest
matches. The timed region is the replay loop alone.

l2book joins through `l2book-xeng` (`xeng.c`), which follows the same
rules as their adapters.

Results: 21 rounds, interleaved, with the engine order rotated each round.

| engine | language | core digest | median replay | fastest | slowest |
|---|---|---|--:|--:|--:|
| **l2book** | C | agrees | **3.85 ms** | 3.83 | 3.92 |
| [CppTrader](https://github.com/chronoxor/CppTrader) | C++ | agrees | 5.75 ms | 5.72 | 5.79 |
| [geseq/orderbook](https://github.com/geseq/orderbook) | Go | agrees | 8.33 ms | 8.11 | 19.72 |
| intrepidkarthi/orderbook | Go | agrees | 13.67 ms | 13.37 | 32.29 |
| [OrderBook-rs](https://github.com/joaquinbejar/OrderBook-rs) | Rust | agrees | 67.96 ms | 67.51 | 68.50 |

- All five engines produce the same digest: 21,856 trades and a
  3,975-order final book.
- Per command, l2book takes 77 ns and intrepidkarthi's engine 273 ns: 3.5x.

## 2. Single operations: their latency scenarios, ported

Their `pkg/matching/latency_scenarios_test.go` and `latency_test.go` time
one operation at a time against a warm book. `bench_scenarios.c` ports
each scenario operation for operation, keeping the same:

- preload size, price formula and order sizes;
- refill rule;
- accounts;
- STP modes;
- timing: one monotonic clock pair around each call, and a nearest-rank
  quantile over the sorted samples. Theirs (`pctl`) takes
  `sorted[int(q * n)]` and `bench_scenarios` takes
  `sorted[int(q * (n-1))]`: at 200,000 samples the two pick ranks at
  most two apart.

Both sides run 200,000 operations per scenario (their
`-benchtime=200000x`), in 5 interleaved rounds. All figures are ns per
operation, median over the rounds.

| scenario | Go p50 | l2book p50 | Go p99 | l2book p99 | Go p99.9 | l2book p99.9 | Go p99.99 | l2book p99.99 |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| CancelHeavy (90% cancel) | 224 | 70 | 2175 | 275 | 2407 | 596 | 4840 (1) | 684 |
| AddOnly | 415 | 88 | 2940 | 272 | 6435 | 581 | 71306 | 679 |
| CancelOnly (201k book) | 178 | 86 | 680 | 151 | 64450 | 191 | 72830 | 446 |
| AggressiveWalk (5-lot sweeps) | 1138 | 308 | 4420 | 577 | 4927 | 851 | 23064 | 1238 |
| Mixed 70/20/10 | 359 | 90 | 2672 | 280 | 4215 | 586 | 50624 | 685 |
| ThinBook (see note) | 343 | to re-measure | 2679 | to re-measure | 6480 | to re-measure | 183325 | to re-measure |
| STP Allow | 286 | 101 | 2177 | 268 | 2310 | 547 | 5266 | 594 |
| STP CancelNewest | 184 | 69 | 227 | 213 | 2098 | 508 | 2290 | 554 |
| STP CancelOldest | 348 | 146 | 2348 | 523 | 2499 | 707 | 5827 | 984 |
| STP CancelBoth | 240 | 102 | 2031 | 261 | 2204 | 543 | 3171 | 599 |
| STP Decrement | 286 | 100 | 2079 | 258 | 2267 | 541 | 5059 | 591 |

(1) Their `BenchmarkLatency_CancelHeavy` (in `latency_test.go`, not the
scenario file) reports p50, p99, p99.9 and max only. For this table one
line was added to it, `b.ReportMetric(float64(pctl(lat, 0.9999)),
"p9999-ns")`, their own `pctl` on their own samples, the same call
their other scenarios make. The 5 rounds gave 3,740-5,344 ns.

- **Medians:** l2book is 2.1x-4.7x faster.
- **Tails:** this is where the two differ most. The Go engine's p99 sits
  around 2-3 us in nearly every scenario, and its p99.99 reaches 23-183 us
  where operations allocate (1-3 allocations per operation in AddOnly,
  Mixed, ThinBook and AggressiveWalk) and in CancelOnly's 201k-order
  book. l2book allocates nothing after `book_init`, and its p99.99 stays
  below 1.3 us in every scenario.
- **ThinBook:** its 5,000 one-lot levels span more than l2book's
  4,096-tick price window. When this table was measured the window did
  not move, so l2book refused the top 904 levels at preload and held a
  smaller book than the Go engine; its figures in this row are struck
  until they are measured again. l2book now accepts every level: the
  904 above the window wait in its overflow store, and `bench_scenarios`
  prints 0 refused.
- **What ThinBook times:** the scenario's taker is a good-till-cancelled
  buy of 10 limited at 3020, the same in their test and in this port.
  The first two take the asks at 3000-3020; every later one finds
  nothing at or below 3020 and rests as a bid. So 199,997 of the
  200,000 timed operations rest an order behind a growing queue at one
  price; they do not walk 10 levels. Both engines run the same
  operations, so the row is still like for like. For a real 10-level
  walk across all 5,000 levels, with the taker priced above the book,
  l2book on the 48-core development host takes about 450 ns at the
  median. The window then moves once every 400 walks or so, and that
  walk takes about 11 us (worst seen: 29 us).

## 3. The venue, over TCP

`e2e.c` is a single load client that speaks both venues' wire protocols:

- **l2book:** its own protocol, `PROTOCOL.md`;
- **obgw:** their gateway, SoupBinTCP framing, protocol v4.

It runs the same flow against each. Each client thread has its own
session and account, and loops: send a resting limit order and wait for
its acknowledgement, then cancel it and wait for that. A latency sample
is one round trip, from socket write to the reply being read (their
obsoak's definition).

The setup:

- **Venues:** pinned to cores 0-15 of NUMA node 0 (memory bound to that
  node), configured as their soak runs them: per-account rate limit
  lifted, no WAL, so neither side syncs a log.
- **l2book:** 4 book shards and the default 2 session-fiber loops, plus
  rows with 8 shards and 1, 2, 4 or 8 fiber loops (`--fiber-loops`).
- **Client:** pinned to cores 32-47, on the other socket (node 1).
- **Rounds:** 3, interleaved across venues and client counts; the table
  shows medians.

| venue | clients | round trips/s | p50 | p99 | p99.9 | p99.99 |
|---|--:|--:|--:|--:|--:|--:|
| **l2book** | 1 | 36,455 | 26.6 us | 30.7 us | 34.8 us | 1.51 ms |
| **l2book** | 4 | 133,890 | 28.7 us | 38.9 us | 43.0 us | 1.57 ms |
| **l2book** | 16 | 166,338 | 90.1 us | 131 us | 1.51 ms | 1.57 ms |
| l2book `--spin-us 0` | 1 | 31,993 | 29.7 us | 34.8 us | 38.9 us | 1.51 ms |
| l2book `--spin-us 0` | 16 | 162,353 | 94.2 us | 131 us | 1.51 ms | 1.64 ms |
| l2book, 8 shards, 1 fiber loop | 16 | 86,261 | 180 us | 229 us | 819 us | 918 us |
| l2book, 8 shards, 2 fiber loops | 16 | 168,974 | 90.1 us | 127 us | 786 us | 885 us |
| l2book, 8 shards, 4 fiber loops | 16 | 323,828 | 34.8 us | 69.6 us | 81.9 us | 918 us |
| **l2book, 8 shards, 8 fiber loops** | 1 | 37,850 | 26.6 us | 30.7 us | 34.8 us | 754 us |
| **l2book, 8 shards, 8 fiber loops** | 4 | 139,571 | 27.6 us | 32.8 us | 36.9 us | 786 us |
| **l2book, 8 shards, 8 fiber loops** | 16 | 481,646 | 30.7 us | 41.0 us | 49.2 us | 852 us |
| obgw, outbound poll 20 us | 1 | 2,262 | 94.2 us | 1.05 ms | 1.05 ms | 1.18 ms |
| obgw, outbound poll 20 us | 4 | 28,091 | 123 us | 1.05 ms | 1.11 ms | 1.51 ms |
| obgw, outbound poll 20 us | 16 | 41,862 | 426 us | 590 us | 1.11 ms | 4.72 ms |
| obgw as shipped | 1 | 500 | 2.10 ms | 2.10 ms | 2.10 ms | 2.10 ms |
| obgw as shipped | 4 | 2,000 | 2.10 ms | 2.23 ms | 2.23 ms | 2.75 ms |
| obgw as shipped | 16 | 7,998 | 2.23 ms | 2.36 ms | 2.49 ms | 2.75 ms |

Measured on libxtc 2.1.1-37 (`a2134a08`) with examples `7748ab9` plus
the fix that makes the order-id map reclaim deleted slots
(backward-shift deletion; before it, a book refused every order once
three quarters of the map had ever been used). Host: a two-socket
64-core x86-64 bare-metal server (Ice Lake, 128 threads, 1 TiB),
Debian 13, kernel 6.12, the same tuning as above; obgw built from
`3fffc25` with Go 1.24.4. Every round of every cell is within 6% of its
median for round trips per second.

**The two sockets.** Every round trip above crosses the socket twice.
A control run with the client on the venue's node (cores 16-31) gives,
for one client, 54,189 round trips/s and an 18.4 us p50 for l2book
(4 shards), 55,545 and 18.4 us for 8 shards and 8 fiber loops, and
2,740 and 69.6 us for obgw with the 20 us poll. With 8 fiber loops and
16 clients the rate is the same either way (482k). A plain blocking TCP
echo of 32 bytes on this host takes 19.1 us across the sockets and 9.9 us
within one (p50, 3 runs of 100,000).

**obgw as shipped.** obgw sends replies by polling each session's outbound
stream every 2 ms (`followStream`, `cmd/obgw/server.go`; their SOAK.md
mentions it). So every round trip takes at least 2 ms, and a closed-loop
client gets exactly 500 round trips per second.

**obgw with a 20 us poll.** For a fairer comparison of the rest of the
path, the second obgw build sets that poll to 20 us. It is the only line
changed. The remaining differences:

- **One client:** p50 is 94 us against l2book's 26.6 us, 3.5x.
- **Throughput:** 4.0x-16x lower than l2book at its default, and 11.5x
  lower with 8 fiber loops at 16 clients.
- **Tail:** p99 and p99.9 stay near 1 ms.

**l2book.** With the client on the venue's node, one client sees
18.4 us, of which the plain echo is 9.9 us, so l2book's own share of a
round trip is about 8.5 us; across the sockets it is 26.6 us against the
echo's 19.1 us, about 7.5 us. That share is:

- decode;
- the hop from the session fiber to the book's shard over a ring;
- matching;
- the hop back;
- encode;
- waking the session fiber, which parks on its socket between orders
  (the echo server does not park).

The 48-core one-socket host measured 17.4 us against a 13 us echo, a
4 us share; this host's share was not broken down further.

**More clients.** The session fibers run on an `xtc_exec` of their own
(`--fiber-loops`, default 2), each connection pinned to one loop. With
8 shards and 16 clients, 1, 2, 4 and 8 fiber loops give 86k, 169k,
324k and 482k round trips/s: each doubling of the loops nearly doubles
the rate, and the p50 falls from 180 us to 31 us. On the default 2
loops the venue levels off at about 166k from 16 clients. Until libxtc
2.2 a pid sent across two `xtc_exec`s reached the wrong process, and
every session fiber ran on one standalone loop, which capped the venue
near 140k. The book shards are not the limit: the slowest book handler
measured is about 0.2 us.

**Busy-polling.** `--spin-us 0` turns off the shards' busy-poll before
parking. That costs 3 us at the median with one client, and less as
load keeps the shards awake.

**The far tail.** Each l2book row's p99.99 is one round trip in
10,000, and it is 0.7-1.6 ms where the p99.9 is tens of microseconds.
With 8 shards (at any fiber-loop count) it is 0.72-0.92 ms; with the
4-shard default it is 1.5-1.6 ms. It is in every round, and in the
control with the client on the venue's node, so it comes from the
venue's side of the host, not from the path between the sockets. Its
cause was not found in this run. The 48-core one-socket host measured
0.38-0.48 ms for the same rows.

## What the numbers do not say

- **What each engine pays for.** intrepidkarthi's engine stamps every
  order with the wall clock, interns account strings, and keeps
  per-account counters. Its gateway runs an admission gate, a rate
  limiter, a resumable sequenced session (SoupBinTCP) and a goroutine per
  connection, and, when asked, a fsync'd WAL with snapshots. They describe
  all of this in their docs.

  l2book's venue has no WAL, no resume, no authentication and no rate
  limit; its README says so. The engine tables (sections 1 and 2) compare
  matching cores that do comparable work per command. The venue table
  compares two programs that do different amounts of it.

- **One tape and one machine.** Section 1 ranks nothing beyond this tape,
  which is the caveat their CROSS-ENGINE.md section 1 states as well.

- **Durability is not measured.** With a WAL that fsyncs every command,
  both venues would be bound by the disk.

## Reproducing it

- **Section 1:** follow their `.github/workflows/xeng.yml`, adding
  `l2book=<l2book-xeng output>` to `xeng table`.
- **Section 2:** run `make bench_scenarios`, then
  `taskset -c N ./bench_scenarios 200000`. For their side, run
  `go test -run '^$' -bench 'BenchmarkLatency_' -benchtime 200000x -cpu 1 ./pkg/matching/`,
  after adding the one `p9999-ns` line to `BenchmarkLatency_CancelHeavy`
  (footnote 1).
- **Section 3:** `e2e.c` and the round driver are not part of the example;
  they are kept with the release notes.
