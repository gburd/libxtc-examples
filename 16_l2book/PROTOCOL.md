# 16_l2book wire protocol

All integers are little-endian.  Prices are int64 ticks (1 tick = $0.01
for US equities), quantities int64 shares.  The protocol is a reference
edge for the example, not a claim of OUCH or ITCH compatibility; its
shapes follow them (fixed-size binary records, one type byte).

## Order entry (TCP, port 9000)

Every message is a 4-byte length prefix (the xtc_net_send_frame
framing), then the record.  The first byte of the record is its type.

Client to venue:

| type | name    | layout after the type byte |
|------|---------|----------------------------|
| `L`  | login   | u32 account, u8[16] token (zero-padded) |
| `O`  | order   | u64 client_id, u16 symbol, u8 side, u8 type, u8 tif, u8 stp, u8 flags, u8 peg, i64 price, i64 stop, i64 qty, i64 display, i64 peg_offset, u64 oco_client_id, i64 expire_ns |
| `X`  | cancel  | u64 client_id, u16 symbol |
| `R`  | reduce  | u64 client_id, u16 symbol, i64 new_qty |
| `U`  | replace | u64 client_id, u16 symbol, u64 new_client_id, i64 price, i64 qty |
| `C`  | cancel all for the account, all symbols | (none) |

Venue to client (the events of book.h, for this account only):

| type | name | layout |
|------|------|--------|
| `A`  | accepted | u64 client_id, u16 symbol, u8 side, i64 price, i64 qty, u64 seq |
| `E`  | executed (a fill, as maker or taker) | u64 client_id, u16 symbol, u8 side, i64 price, i64 qty, u8 liquidity (`A` added, `R` removed), u64 seq |
| `D`  | done (cancelled / expired / IOC rest) | u64 client_id, u16 symbol, i64 leaves, u8 reason, u64 seq |
| `J`  | rejected | u64 client_id, u16 symbol, u8 reason, u64 seq |
| `M`  | reduced  | u64 client_id, u16 symbol, i64 leaves, u64 seq |
| `P`  | replaced | u64 client_id, u16 symbol, u64 new_client_id, u64 seq |

Order ids: the venue keeps (account, client_id) -> venue id per symbol;
a client never names another account's order.

## Market data (TCP, port 9001, and SSE on the web port)

Binary feed, framed as above, venue to subscriber:

| type | name | layout |
|------|------|--------|
| `S`  | snapshot | u16 symbol, u64 seq, u16 n_bids, u16 n_asks, then (i64 price, i64 qty) x (n_bids + n_asks) |
| `B`  | level    | u16 symbol, u8 side, i64 price, i64 qty (0 = removed), u64 seq |
| `T`  | trade    | u16 symbol, u8 aggressor side, i64 price, i64 qty, u64 seq |
| `H`  | phase    | u16 symbol, u8 phase, u64 seq |

A subscriber gets the snapshot, then every change after its sequence:
snapshot + deltas = the book.  The SSE stream on the web port carries
the same events as JSON lines (`event: book|level|trade|phase|stats`).

## Web (HTTP, port 8080)

| path | what |
|------|------|
| `/` | the landing page (web/index.html); any other path is a file under `--web-root` |
| `/console.html` | the console: LIVE (this venue's books) or SIM (book.wasm) |
| `/book.wasm` | the engine and the console's market, for the pages |
| `/symbols` | JSON: the symbols (the console's LIVE probe) |
| `/md?sym=AAPL` | SSE market data for one symbol |
| `/order` | POST (query string) an order as the "you" account: `sym, side, type, price, qty, tif` |
| `/stats` | JSON: per-shard and per-stage latency percentiles, throughput |
| `/healthz`, `/readyz` | liveness, and "books are up and quoted" |
