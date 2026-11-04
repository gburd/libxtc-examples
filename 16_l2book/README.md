# 16_l2book -- a level-2 limit order book on libxtc

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
file compiles to a 23 KB freestanding `wasm32` module: the browser
console's LOCAL mode runs the engine the venue runs.

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
point on the money path, no allocation after the book is created.

## Why it is fast

- **Price ladder, not a tree.**  Each side is a dense array of 4096 levels
  (+-$20 around the touch at a penny tick) under a two-level bitmap: best
  bid and ask, and the next level, are a find-first-set each.
- **Pointer-free pool.**  Orders live in a fixed pool indexed by `uint32`,
  linked FIFO per level; an open-addressed id map gives O(1) cancel.
- **One writer per book, no locks.**  Cross-core traffic is one SPSC ring
  per (sender, shard), and a busy shard costs its senders no system call:
  a sender writes the wake byte only to a shard that is parking.
- **Busy-poll when idle** (`--spin-us`, default 50): a shard keeps checking
  its rings before it parks, so a burst never pays a wake.

## Numbers (this host, measured; regenerate on yours)

| what | how | result |
|---|---|---|
| bare engine | `make bench` (one pinned core, warm 20k-order book, -O3) | cancel-heavy 28.2 M ops/s, p50 49 ns, p99 204 ns, p999 480 ns; mixed 7.3 M ops/s, p50 124 ns |
| book handler | `l2book --bench`, the Isolate's `book_*` call | p50 ~140 ns, p99 ~500 ns |
| session -> book hop | ingress timestamp to handler start | p50 0.6 us (idle shard, spinning); 3-9 us parked |
| round trip | 4 TCP clients, order + cancel, loopback | p50 45 us, p99 96 us; the kernel's own loopback TCP echo is p50 21 us |

Per-operation figures include a ~18 ns `clock_gettime` pair; the median to
tail shape is the signal.  None includes a log: there is no WAL here.

## Run it

```
make XTC_BUILD=../../build_unix check     # engine tests, venue self-test
make bench                                 # the bare engine
./l2book --flow                            # venue + flow; console on :8080
./l2book --bench 20000                     # loopback round-trip latency
make wasm WASM_CC=clang                    # web/book.wasm for LOCAL mode
```

A running market in one command, from the libxtc checkout root:

```
docker compose -f examples/16_l2book/compose.yaml up --build
open http://127.0.0.1:8080/
```

`venue` builds libxtc and the example from one tree and runs the venue
with its built-in market maker and noise flow; `flow` is `l2book-flow`, an
outside client trading through the real order-entry port.
`smoke.sh "docker compose"` (or `"podman compose"`) brings it up and
checks it: ready, every book quoted, trades rising, the console, book.wasm
and SSE served, and a restart that comes back.

## Tests

- `test_book`: 17 unit cases, a differential test of 200,000 random
  commands against `refbook.h` (a naive reference matcher sharing no code),
  and a determinism check.  Clean under ASan and UBSan.  Three planted bugs
  (LIFO within a level, an iceberg refill keeping its place, stops on the
  wrong side) each fail it.
- `l2book --self-test`: two TCP sessions and a market-data subscriber
  against a running venue -- rest, cross (both fills), ownership, cancel,
  level and trade on the feed, cancel-on-disconnect.
- `smoke.sh`: the compose box, end to end.

## What it found in libxtc

Writing this example found two libxtc bugs, both now addressed in libxtc:
an `xtc_tnt_send` to an Isolate on another shard wrote that shard's
mailbox from the sender's thread and lost messages (fixed: per-pair
rings, and `xtc_tnt_send_from` for senders outside a handler); and a pid
is executor-relative, so a book's `xtc_send` to a fiber on a second
`xtc_exec` reached the proc with the same slot on the book's exec.  The
fibers therefore run on one standalone loop, whose pids resolve from
anywhere; see `fibers.c`.

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
| `wasm.c`, `web/index.html` | the browser exports and the console |
| `PROTOCOL.md` | order entry, market data and web endpoints |
| `Dockerfile`, `compose.yaml`, `smoke.sh` | the box |
