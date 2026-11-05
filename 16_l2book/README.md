# 16_l2book -- a level-2 limit order book on libxtc

**[Live console](https://gburd.github.io/libxtc-examples/l2book/console.html)**
([landing page](https://gburd.github.io/libxtc-examples/l2book/)): this
example's engine, compiled to a 50 KB WebAssembly module, running a market
in your browser.  The console is a reproduction of
[intrepidkarthi/orderbook's console](https://intrepidkarthi.github.io/orderbook/console.html)
on this engine, with the same panels, the same noise-trader market and
the same signals and detectors, ported to C.  A seed opens the same market
on both pages, print for print (`web/README.md`).  `docker compose up`
adds the console's LIVE mode: the venue's own books (below).

A US-equities limit order book and matching venue that uses libxtc's two
concurrency models together, each where it fits:

- **Isolates for the books.**  Each symbol's book is a stackless
  `xtc_tnt` Isolate on the shard (core) the symbol hashes to.  It is the
  only writer of its book (the LMAX single-writer model: no lock on the
  match path), its handler runs one command to completion and returns, and
  it never blocks or allocates.  An Isolate is a few hundred bytes in a
  dense typed arena, dispatched in batches; the shard is the only fiber.
- **Fibers for everything that blocks.**  Each TCP order-entry session,
  each market-data subscriber, each browser (HTTP + SSE), and the flow
  generator is an `xtc_proc`: straight-line code that reads, decodes,
  sends a command to a book, and parks.  `xtc_proc_wait_fd` wakes on the
  socket *or* the fiber's mailbox, so one fiber both reads orders and
  writes back the execution reports the book sends it.

```
 TCP order entry --> session fiber --\                     +--> exec reports -> session mailbox
 market data     --> md fiber        +-- xtc_tnt_send_from --> book Isolate (1 per symbol,
 browser (SSE)   --> web fiber       |   (per-shard ring)       single writer, book.c)
 built-in flow   --> flow fiber -----/                     +--> L2 levels / trades -> md subscribers
```

The engine (`book.c`) is pure C11 with no libc beyond `mem*`, so the same
file compiles to a freestanding `wasm32` module (50 KB with the console's
market, signals and detectors, no imports): the browser console runs the
engine the venue runs.

## Features

The order surface of [intrepidkarthi/orderbook](https://github.com/intrepidkarthi/orderbook),
in C: limit, market, stop, stop-limit, trailing stop, iceberg (the refill
goes to the back of the queue), one-cancels-other, pegged to bid / ask /
mid plus an offset (priced at entry); GTC, IOC, FOK, DAY, GTD; post-only;
cancel, reduce keeping queue priority, replace losing it, cancel-all for
an account, cancel-on-disconnect; price-time FIFO or pro-rata at a level;
five self-trade-prevention modes; pre-open, continuous, halted,
cancel-only and closed sessions with a uniform-price opening auction
(maximum volume, then minimum imbalance, then nearest the reference); a
price-band circuit breaker; L2 level events, trades, and a book digest
that two equal books share.

Prices are int64 ticks (cents) and quantities int64 shares: no floating
point on the money path, no allocation after the book is created.  Any
positive whole number of ticks is a valid price; the price band is a
separate rule the venue turns on.

## Why it is fast

- **Price ladder, not a tree.**  Each side is a dense window of 4096
  levels (+-$20 at a penny tick) under a two-level bitmap: best bid and
  ask, and the next level, are a find-first-set each.
- **The window follows the book.**  A price outside the window is not
  refused.  Its level goes into a small overflow store: per side, an
  array of levels sorted by price.  The best-price search and a sweep
  fall through to it only when the window runs out on that side.  At the
  end of each command the window moves if it has to (a re-base): onto
  the whole book when every level fits in 4096 ticks, and otherwise onto
  the touch, leaving the far levels in the overflow.  A re-base copies
  the occupied levels and shifts the 64 bitmap words; orders hold prices,
  not ladder indices, so no order is touched, and L2 events carry
  prices, so nothing outside the engine can see it.  A US-equity book
  near its touch never leaves the window; there the overflow costs a few
  well-predicted tests per command (about 30 instructions on
  `bench_book`'s flows, +5.7%, with no wall-time difference this host
  could resolve).  A re-base over a 5,000-level book takes about 11 us.
  The overflow is sized at `book_init` from `max_orders` (every level
  holds at least one order), so it never allocates: 40 bytes per order.
- **Pointer-free pool.**  Orders live in a fixed pool indexed by `uint32`,
  linked FIFO per level; an open-addressed id map gives O(1) cancel.
- **One writer per book, no locks.**  Cross-core traffic is one SPSC ring
  per (sender, shard), and a busy shard costs its senders no system call:
  a sender writes the wake byte only to a shard that is parking.
- **Busy-poll when idle** (`--spin-us`, default 50): a shard keeps checking
  its rings before it parks, so a burst never pays a wake.

## Numbers (this host, measured; regenerate on yours)

Measured on libxtc 2.1.1-37 (`a2134a08`), examples `7748ab9` plus the
order-id map fix, on a two-socket 64-core x86-64 bare-metal host (Xeon
Ice Lake, 128 threads), Debian 13, everything on NUMA node 0; medians of
5 runs (`make bench`) and 3 runs (the rest).

| what | how | result |
|---|---|---|
| bare engine | `make bench` (one pinned core, warm 20k-order book, -O3) | cancel-heavy 18.9 M ops/s, p50 59 ns, p99 160 ns, p999 288 ns; mixed 7.8 M ops/s, p50 116 ns; sweep 8.3 M ops/s, p50 108 ns |
| book handler | `l2book --bench`, the Isolate's `book_*` call | p50 ~105 ns, p99 ~600 ns |
| session -> book hop | ingress timestamp to handler start, `l2book --bench` | p50 0.66 us, p99 0.85 us (spinning shard); p50 4.2-4.4 us with `--spin-us 0` (parked shard) |
| round trip | `l2book --bench`: 4 TCP clients, order + cancel, loopback | 174k round trips/s, p50 22 us, p99 27 us; a plain loopback TCP echo is p50 9.9 us on this socket |

Per-operation figures include a ~18 ns `clock_gettime` pair; the median to
tail shape is the signal.  None includes a log: there is no WAL here.

Against [intrepidkarthi/orderbook](https://github.com/intrepidkarthi/orderbook),
the engine this one takes its feature set from, on one bare-metal host
(COMPARISON.md has the method and every number):

- **Same tape, same checksum.** On their cross-engine tape (50,000
  commands) all five engines produce the same OBDG digest. Median replay
  time: l2book 3.85 ms, CppTrader 5.75, geseq 8.33, theirs 13.67,
  OrderBook-rs 68.0.
- **Their latency scenarios, ported operation for operation.** l2book's
  medians are 2.1x-4.7x lower. Its p99.99 stays below 1.3 us in every
  scenario; theirs reaches 23-183 us wherever an operation allocates.
- **End to end over loopback TCP, one closed-loop client** (client on
  the other socket). l2book's p50 is 26.6 us (36k round trips/s); with
  8 shards, 8 fiber loops and 16 clients it carries 482k round trips/s
  at a 31 us p50. Their gateway as shipped is 2.1 ms (500 round
  trips/s), because it polls each session's outbound stream every 2 ms.
  With that poll set to 20 us it is 94 us. The two venues do not do the
  same work per order; see COMPARISON.md.

## Run it

```
make XTC_BUILD=../../build_unix check     # engine + console tests, venue self-test
make bench                                 # the bare engine
./l2book --flow                            # venue + flow; pages on :8080
./l2book --bench 20000                     # loopback round-trip latency
make wasm WASM_CC=clang                    # web/book.wasm for the console
make check-wasm                            # book.wasm against known answers (node)
```

A running market in one command, from the libxtc checkout root:

```
docker compose -f examples/16_l2book/compose.yaml up --build
open http://127.0.0.1:8080/console.html
```

`venue` builds libxtc and the example from one tree and runs the venue
with its built-in market maker and noise flow; `flow` is `l2book-flow`, an
outside client trading through the real order-entry port.
`smoke.sh "docker compose"` (or `"podman compose"`) brings it up and
checks it: ready, every book quoted, trades rising, the landing page, the
console, book.wasm, `/symbols` and SSE served, and a restart that comes back.

The console served by the venue starts in LIVE mode (the switch in its
bar lists every symbol, and SIM): the ladder and tape are the venue's
SSE feed, "you" trades through `/order`, and the console's signal code
runs over what the venue publishes.  The spoof and flood buttons, the
seed and "your resting orders" are SIM only.  The feed is level 2 with no
order ids, and the web port has no open-orders endpoint.

## Tests

- `test_book`: 24 unit cases, two differential tests of 200,000 random
  commands each against `refbook.h` (a naive reference matcher sharing no
  code, with no price window), and a determinism check.  The first
  differential keeps prices within +-20 ticks; the second spreads them
  over +-20,000 ticks, ten windows wide, so the window moves 32,889 times
  and the overflow holds up to 26 levels, and neither may change a fill.  The unit cases include prices
  far outside the first window, a 20,000-tick trend up and back,
  intrepidkarthi's ThinBook (5,000 one-lot levels), the opening auction
  over a book wider than the window, and two books with different
  re-base histories that must emit the same events and digests.  Clean
  under ASan and UBSan.  Planted bugs each fail it: LIFO within a level,
  an iceberg refill keeping its place, stops on the wrong side, and
  `-DPLANT_REBASE_NOBITMAP` (a re-base that does not shift one bitmap
  word).
- `test_signals`: the console's market, signals and detectors against
  known answers: intrepidkarthi's own signal and surveillance test vectors,
  Go's `math/rand` golden values, their console's output for seed 1 (79
  and 1,000 steps, and a spoof / flood / "you" run with its 9 alerts), and
  a seed replay.  `check_wasm.mjs` runs the same answers against
  `book.wasm` before every Pages deploy.
- `l2book --self-test`: two TCP sessions and a market-data subscriber
  against a running venue -- rest, cross (both fills), ownership, cancel,
  level and trade on the feed, cancel-on-disconnect.
- `smoke.sh`: the compose box, end to end.

## What it found in libxtc

Writing this example found two libxtc bugs, both now addressed in libxtc:
an `xtc_tnt_send` to an Isolate on another shard wrote that shard's
mailbox from the sender's thread and lost messages (fixed: per-pair
rings, and `xtc_tnt_send_from` for senders outside a handler); and a pid
was executor-relative, so a book's `xtc_send` to a fiber on a second
`xtc_exec` reached the proc with the same slot on the book's exec (fixed
in 2.2: a pid now names its loop by a process-wide id).  Until the fix
the fibers ran on one standalone loop, which capped the venue near 140k
round trips/s; they now run on an `xtc_exec` of their own
(`--fiber-loops`, default 2), each connection pinned to one loop.  With
8 fiber loops, 8 shards and 16 clients on a two-socket 64-core host:
482k round trips/s, p50 31 us (1, 2 and 4 loops: 86k, 169k, 324k; see
`COMPARISON.md`).

## Files

| file | what |
|---|---|
| `book.c`, `book.h` | the matching engine (pure C; native and wasm32) |
| `refbook.h`, `test_book.c` | the reference matcher and the engine tests |
| `bench_book.c`, `hist.h` | the bare-engine benchmark; a log-linear histogram |
| `venue.c`, `venue.h` | the book Isolates and the boot |
| `fibers.c` | sessions, market data, the web console, the flow, as fibers |
| `main.c` | options, `--self-test`, `--bench` |
| `flow.c` | `l2book-flow`, the outside order-flow client |
| `console.c`, `signals.c`, `gorand.c` | the console's market, signals and detectors; Go's `math/rand` |
| `test_signals.c`, `check_wasm.mjs` | their tests, native and against `book.wasm` |
| `wasm.c`, `web/` | the browser exports; the landing page and the console (`web/README.md`) |
| `PROTOCOL.md` | order entry, market data and web endpoints |
| `xeng.c` | `l2book-xeng`, the adapter for intrepidkarthi's cross-engine harness |
| `bench_scenarios.c` | their per-operation latency scenarios, ported |
| `COMPARISON.md` | the comparison with intrepidkarthi/orderbook |
| `Dockerfile`, `compose.yaml`, `smoke.sh` | the box |
