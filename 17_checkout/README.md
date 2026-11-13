# 17_checkout -- a checkout service: saga, connection pool, request context

`checkout.c` runs six orders through a checkout that spans three systems
which cannot share a transaction: inventory, payments and shipping.  Each
order is an `xtc_proc`; each checkout is an `xtc_saga`; the backend
connections come from an `xtc_pool`; and the request's id, trace id and
connection live in the order proc's dictionary (`xtc_pdict`).

```sh
make 17_checkout && ./17_checkout          # from a libxtc build tree
make XTC_BUILD=../../build_unix test       # from this directory
```

Three orders go through.  Three fail at a different step each, so each
failure unwinds a different prefix:

```
order 1004 dave: 3 x widget to Antarctica: FAILED at ship (no carrier) after 3 completed steps
  [8191422c] quote: 3 x widget = $37.50
  [8191422c] reserve 3 x widget: ok
  [8191422c] charge dave $37.50: ok
  [8191422c] ship to Antarctica: no carrier serves it
  [8191422c] refund dave $37.50
  [8191422c] release 3 x widget
```

At the end the program checks the ledger: stock, balances, shipments,
charges and refunds are what the successful orders alone imply, the pool
reached its limit of two connections without exceeding it, four orders
had to queue for a connection, and every connection came back.
`expected.txt` is the full output; `make check` in the libxtc build tree
runs the program and diffs its output against it.

## What it shows

**A saga, not a distributed transaction.**  A checkout cannot hold a
lock on the stock while the card network answers, and the three services
do not share a commit protocol.  The saga's steps are ordinary C:

| step | forward | compensation |
|---|---|---|
| quote | price the order | none (a read) |
| reserve | take the stock | put it back |
| charge | charge the card | refund it |
| ship | book the carrier | none (the last step) |

`xtc_saga_run` runs the steps in order on the order's own fiber.  When one
fails it runs the compensations of the steps that completed, newest first,
and returns the failing step's error code.  `xtc_saga_failed_step` names
the step; the order reports it.  The last step has no compensation, and
that is correct: a compensation runs only when a later step fails.

**A pool as a concurrency limit.**  The backend accepts two connections
and six orders are in flight.  `xtc_pool_checkout` parks the order's fiber
(it does not block the loop's thread) until a connection is checked in.
The pool owns no connections; it only tracks which are free, so the
program creates them, adds them, and frees them after
`xtc_pool_destroy`.

**Request context without threading it through every call.**  The saga
step functions take one `void *`, the order.  The backend client
(`db_roundtrip`) and the trace helper (`trace`) need the connection and
the trace id, and they get them from the proc's dictionary:
`xtc_pdict_get("conn", ...)`.  A threaded server would use thread-local
storage for this; here it would be wrong, because all six orders run on
the loop's one thread and interleave at every round trip.  The dictionary
is per proc.  The order clears it with `xtc_pdict_clear` when the request
ends; a long-lived worker that serves many requests has to do that anyway.

## Design choices

* **One saga per order, built per request.**  A saga runs at most once,
  and building one is a handful of appends, so a request builds its own
  rather than sharing a template.
* **The order is the steps' context.**  Each step reads what it needs and
  records what its compensation will undo (`amount` from the quote, the
  quantity reserved).  Nothing is captured in globals per order.
* **The steps report why they failed.**  `xtc_saga_run` returns an
  `XTC_E_*` code, which is not something to show a customer.  The step
  sets `why` before it returns, and the order prints it.
* **Error codes chosen by meaning.**  Out of stock is `XTC_E_RESOURCE`, a
  declined card `XTC_E_ABORTED`, an unserved destination `XTC_E_INVAL`.
  The caller can branch on them without parsing text.
* **No locks around the backend's state.**  Every order proc runs on one
  loop, and a proc runs until it parks, so each check-then-update in a
  step (stock, balance) happens with no other order in between.  Spread
  the orders over an `xtc_exec` and the backend would need its own
  locking, as a real one has.

## Alternatives not chosen

* **Two-phase commit.**  It needs every participant to speak a prepare
  protocol and to hold locks across the network round trip; payment
  networks do not, and holding stock locks for seconds stalls every other
  buyer.  The saga gives up atomic visibility (another order can see the
  reserved stock before the charge fails) for availability.
* **A compensation stack written by hand.**  Every service that does this
  ends up with an array of undo callbacks and an index; `xtc_saga` is that
  array, plus the reverse walk, plus the case that a compensation itself
  fails (it keeps unwinding and returns `XTC_E_INTERNAL`, so the caller can
  tell a clean rollback from a broken one).
* **A semaphore plus a free list instead of the pool.**  That is what
  `xtc_pool` is inside; using it saves writing the free list and the
  "checked in twice" check (`xtc_pool_checkin` refuses a resource that is
  not checked out).
* **Passing a context struct to every function.**  It works, and for a
  small program it is clearer.  It stops working when the code that needs
  the context is a library callback with a fixed signature, which is where
  per-request storage earns its place.  The saga's `xtc_saga_fn` is such a
  signature.
