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

- **CPU:** a 48-core / 96-thread x86-64 server part (Sapphire Rapids),
  one socket, one NUMA node.
- **OS:** Debian 13, kernel 6.12.
- **Tuning:** CPU governor set to `performance`, transparent huge pages
  off, idle states C1E and C6 disabled, NUMA balancing off, and nothing
  else running.
- **Compilers:** gcc 14.2, Go 1.24.4, Rust 1.99.1.

Each engine runs pinned to one core.

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
| **l2book** | C | agrees | **2.76 ms** | 2.73 | 2.92 |
| [CppTrader](https://github.com/chronoxor/CppTrader) | C++ | agrees | 4.26 ms | 4.24 | 4.36 |
| [geseq/orderbook](https://github.com/geseq/orderbook) | Go | agrees | 5.93 ms | 5.85 | 10.62 |
| intrepidkarthi/orderbook | Go | agrees | 9.60 ms | 9.54 | 13.26 |
| [OrderBook-rs](https://github.com/joaquinbejar/OrderBook-rs) | Rust | agrees | 54.88 ms | 54.61 | 55.74 |

- All five engines produce the same digest: 21,856 trades and a
  3,975-order final book.
- A second set of 21 rounds, pinned to a different core, gave the same
  medians to within 0.03 ms.
- Per command, l2book takes 55 ns and intrepidkarthi's engine 192 ns: 3.5x.

## 2. Single operations: their latency scenarios, ported

Their `pkg/matching/latency_scenarios_test.go` and `latency_test.go` time
one operation at a time against a warm book. `bench_scenarios.c` ports
each scenario operation for operation, keeping the same:

- preload size, price formula and order sizes;
- refill rule;
- accounts;
- STP modes;
- timing: one monotonic clock pair around each call, and their quantile
  rule (`sorted[int(q * (n-1))]`).

Both sides run 200,000 operations per scenario (their
`-benchtime=200000x`), in 5 interleaved rounds. All figures are ns per
operation, median over the rounds.

| scenario | Go p50 | l2book p50 | Go p99 | l2book p99 | Go p99.9 | l2book p99.9 | Go p99.99 | l2book p99.99 |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| CancelHeavy (90% cancel) | 157 | 43 | 1490 | 90 | 1591 | 116 | - | 153 |
| AddOnly | 255 | 63 | 1875 | 135 | 4575 | 168 | 48201 | 215 |
| CancelOnly (201k book) | 160 | 81 | 1578 | 145 | 1819 | 181 | 28803 | 224 |
| AggressiveWalk (5-lot sweeps) | 857 | 196 | 2920 | 431 | 3422 | 585 | 9874 | 742 |
| Mixed 70/20/10 | 241 | 61 | 1809 | 130 | 3001 | 166 | 35553 | 209 |
| ThinBook (10-level walks) | 224 | 67 | 1724 | 114 | 4810 | 155 | 113762 | 334 |
| STP Allow | 183 | 72 | 1530 | 92 | 1582 | 118 | 2663 | 229 |
| STP CancelNewest | 115 | 62 | 151 | 102 | 1444 | 143 | 1518 | 251 |
| STP CancelOldest | 231 | 90 | 1538 | 112 | 1607 | 123 | 3117 | 212 |
| STP CancelBoth | 153 | 74 | 1386 | 94 | 1443 | 145 | 1545 | 279 |
| STP Decrement | 185 | 72 | 1451 | 92 | 1496 | 109 | 1583 | 206 |

- **Medians:** l2book is 1.9x-4.4x faster.
- **Tails:** this is where the two differ most. The Go engine's p99 sits
  around 1.5 us in nearly every scenario, and its p99.99 reaches 28-114 us
  where operations allocate (1-3 allocations per operation in AddOnly,
  Mixed, ThinBook and AggressiveWalk). l2book allocates nothing after
  `book_init`, and its p99.99 stays below 0.75 us in every scenario.
- **ThinBook:** its 5,000 one-lot levels span more than l2book's
  4,096-tick ladder, so l2book refuses the top 904 levels at preload.
  `bench_scenarios` prints that count rather than hiding it. A US-equity
  book does not span $50 at a penny tick, and the ladder re-centres when
  the touch moves.

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

- **Venues:** pinned to cores 0-15, configured as their soak runs them:
  per-account rate limit lifted, no WAL, so neither side syncs a log.
- **l2book:** 4 book shards.
- **Client:** pinned to cores 32-47.
- **Rounds:** 3, interleaved across venues and client counts; the table
  shows medians.

| venue | clients | round trips/s | p50 | p99 | p99.9 | p99.99 |
|---|--:|--:|--:|--:|--:|--:|
| **l2book** | 1 | 55,053 | 17.4 us | 18.4 us | 21.5 us | 377 us |
| **l2book** | 4 | 133,548 | 20.5 us | 45.1 us | 49.2 us | 410 us |
| **l2book** | 16 | 139,560 | 106 us | 147 us | 410 us | 475 us |
| l2book `--spin-us 0` | 1 | 44,556 | 21.5 us | 28.7 us | 30.7 us | 393 us |
| obgw, outbound poll 20 us | 1 | 2,865 | 74 us | 1.05 ms | 1.05 ms | 1.11 ms |
| obgw, outbound poll 20 us | 4 | 30,147 | 119 us | 786 us | 1.11 ms | 1.11 ms |
| obgw, outbound poll 20 us | 16 | 38,388 | 475 us | 590 us | 1.11 ms | 3.28 ms |
| obgw as shipped | 1 | 500 | 2.10 ms | 2.10 ms | 2.23 ms | 2.23 ms |
| obgw as shipped | 4 | 2,000 | 2.10 ms | 2.23 ms | 2.36 ms | 2.62 ms |
| obgw as shipped | 16 | 7,999 | 2.23 ms | 2.36 ms | 2.49 ms | 2.75 ms |

**obgw as shipped.** obgw sends replies by polling each session's outbound
stream every 2 ms (`followStream`, `cmd/obgw/server.go`; their SOAK.md
mentions it). So every round trip takes at least 2 ms, and a closed-loop
client gets exactly 500 round trips per second.

**obgw with a 20 us poll.** For a fairer comparison of the rest of the
path, the second obgw build sets that poll to 20 us. It is the only line
changed. The remaining differences:

- **One client:** p50 is 74 us against l2book's 17.4 us, 4.2x.
- **Throughput:** 2.8x-19x lower than l2book.
- **Tail:** p99 and p99.9 stay near 1 ms.

**l2book.** One client sees 17.4 us. A plain loopback TCP echo on this
host costs about 13 us of that, so l2book's own share of a round trip is
about 4 us:

- decode;
- the hop from the session fiber to the book's shard over a ring;
- matching;
- the hop back;
- encode.

**More clients.** l2book's throughput levels off at about 140,000 round
trips per second from 4 clients up. The cause is that one standalone loop
ran every session fiber: a workaround for a libxtc bug (a pid sent across
two `xtc_exec`s reached the wrong process). With the bug fixed (libxtc
2.2), the sessions run on an `xtc_exec` of their own (`--fiber-loops`).
On the 48-core development host, with 8 shards and 16 clients, 1/2/4/8
fiber loops give 54k/149k/313k/383k round trips/s, so the ceiling moves
with the loops. That run was not on the bare-metal host above, and is not
part of the tables. The book shards are not the limit: the slowest book
handler measured is about 0.2 us.

**Busy-polling.** `--spin-us 0` turns off the shards' busy-poll before
parking. That costs 4 us at the median with one client, and less as load
keeps the shards awake.

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
  `go test -run '^$' -bench 'BenchmarkLatency_' -benchtime 200000x -cpu 1 ./pkg/matching/`.
- **Section 3:** `e2e.c` and the round driver are not part of the example;
  they are kept with the release notes.
