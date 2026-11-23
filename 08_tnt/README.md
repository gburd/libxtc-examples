# tnt -- the canonical echo demo for the libxtc Isolate layer

`tnt` is a thread-per-core, shared-nothing, **stackless Isolate** layer
that is now a **supported part of libxtc** (L4 orchestration).  You
write simple, synchronous state machines (Isolates) that react to
messages and return **transitions**; the layer's scheduler runs them,
batches them by type in dense arenas, references them by generational
handle, and commits their I/O effects through libxtc's reactor.

The implementation lives in the library:

- `src/orc/tnt.c` -- scheduler, typed arenas, generational handles,
  effect interpreter, I/O couriers.
- `src/inc/xtc_tnt.h` -- the public header (the whole API).
- `man/man3/xtc_tnt.3` -- the manual page.

It reproduces the developer experience of [Tina](https://github.com/pmbanugo/tina)
(Odin) on libxtc's machinery, at ~hundreds of bytes per Isolate (a dense
typed arena struct) rather than ~4 KiB per stackful fiber -- only the
shard is a fiber.  The design is in `man xtc_tnt` and the
documentation site's Isolate layer section.

This directory, `examples/08_tnt/`, is now just the **canonical demo**:
a TCP echo server that links the in-library tnt and `#include`s
`<xtc_tnt.h>`.

## The example: a TCP echo server

Mirrors Tina's README echo.  Each connection is its own Isolate; no
shared socket table, no lock.

```c
#include "xtc_tnt.h"

typedef struct echo_conn {
	int     fd;
} echo_conn_t;

/* Initialize: record the fd and submit the first recv. */
static xtc_tnt_transition_t
echo_init(void *self_raw, const void *args, size_t args_size)
{
	echo_conn_t *self = xtc_tnt_self_as(echo_conn_t, self_raw);
	const echo_args_t *a = args;
	self->fd = a->client_fd;
	if (xtc_tnt_submit_recv(self->fd) != XTC_TNT_IO_OK)
		return xtc_tnt_transition_to_crash(
		    XTC_TNT_FAULT_CONTRACT_VIOLATION);
	return XTC_TNT_TRANSITION_WAIT_IO;
}

/* Handle I/O completions synchronously.  If anything fails: let it crash. */
static xtc_tnt_transition_t
echo_handler(void *self_raw, xtc_tnt_message_t *msg)
{
	echo_conn_t *self = xtc_tnt_self_as(echo_conn_t, self_raw);
	void *buf;
	switch (msg->tag) {
	case XTC_TNT_IO_TAG_RECV_COMPLETE:
		if (msg->body.io.result <= 0) {
			(void)xtc_tnt_submit_close(self->fd);
			return XTC_TNT_TRANSITION_WAIT_IO;
		}
		/* Keep the reactor buffer and send it back: no copy. */
		buf = xtc_tnt_recv_take(msg);
		if (buf == NULL || xtc_tnt_io_send_owned(self->fd, buf,
		    (size_t)msg->body.io.result) != XTC_TNT_IO_OK) {
			xtc_free(buf);
			return xtc_tnt_transition_to_crash(
			    XTC_TNT_FAULT_CONTRACT_VIOLATION);
		}
		return XTC_TNT_TRANSITION_WAIT_IO;
	case XTC_TNT_IO_TAG_SEND_COMPLETE:
		(void)xtc_tnt_submit_recv(self->fd);
		return XTC_TNT_TRANSITION_WAIT_IO;
	case XTC_TNT_IO_TAG_CLOSE_COMPLETE:
		return XTC_TNT_TRANSITION_DONE;
	default:
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}
}
```

The echoed bytes are never copied: `xtc_tnt_recv_take` keeps the
courier's receive buffer (otherwise freed when the handler returns) and
`xtc_tnt_io_send_owned` sends that block, which the runtime frees once
the send completes.  The plain `xtc_tnt_io_send` instead needs a buffer
inside the Isolate, which would mean copying each chunk into it.

A plain listener proc accepts connections and spawns one EchoConnection
Isolate per connection (round-robin across shards) via
`xtc_tnt_spawn_on`.

## Architecture

The decisive choice is to map a **Shard to one
long-lived libxtc proc** per `xtc_exec` loop -- NOT an Isolate to a
proc.

```
+--------------------------------------------------+
|                  SHARD (one xtc_proc)            |
|                                                  |
|  Typed arenas (one per Isolate type)             |
|   [Echo][Echo][Echo]...   [Timer][Timer]...      |
|   slab-carved at boot, generational slots        |
|                                                  |
|  Dispatch loop (the shard fiber):                |
|    drain inbox                                   |
|    -> collect reactor completions                |
|    -> for each ready type, for each ready slot   |
|       (budgeted): handler -> interpret transition|
|                                                  |
|  Turn-scratch bump arena (reset per handler)     |
|  Completion ring (fed by I/O courier fibers)     |
|  Self-wake pipe                                  |
+--------------------------------------------------+
```

Handlers **never block**.  Actions (send, spawn, submit I/O, arm a
timer) are staged via ambient `xtc_tnt_*` calls during the handler; the
returned transition tells the shard how to commit them.  Only the shard
is a fiber; Isolates are stackless arena structs.

### How I/O works on libxtc

When a handler stages `xtc_tnt_submit_recv` / `xtc_tnt_io_send` /
`xtc_tnt_submit_close` and returns `XTC_TNT_TRANSITION_WAIT_IO`, the shard commits
each staged op by spawning a short-lived **courier** fiber.  The courier
parks on the fd (`xtc_proc_wait_fd`), performs the non-blocking syscall,
posts a completion record to the shard's ring, and wakes the shard.  The
courier is the shard's I/O mechanism -- not an Isolate.  The Isolate
stays a stackless arena struct; only the courier (bounded by the number
of outstanding ops) carries a stack.  This uses purely public libxtc
APIs (`xtc_proc_*`, `xtc_io_*`, `xtc_net_*`).

## What maps from Tina

| Tina concept | tnt |
|---|---|
| `handler(self, msg) -> Effect` | `xtc_tnt_handler_fn` returning `xtc_tnt_transition_t` |
| 64-bit generational handle (shard:type:slot:gen 8:8:20:28) | `xtc_tnt_handle_t`, same bit layout |
| stage-then-commit turn frame (`ctx_*`) | ambient `xtc_tnt_*` calls valid only during a handler |
| transition kinds DONE/YIELD/WAIT_MESSAGE/WAIT_IO/CRASH | `xtc_tnt_transition_kind_t` |
| bounded mailbox, drop-on-full with sender feedback | `xtc_tnt_send` returns `XTC_TNT_SEND_MAILBOX_FULL` |
| typed dense arenas, batched dispatch by type | per-type arena + budgeted per-type scan |
| spawn returns a handle synchronously, init runs now | `xtc_tnt_spawn` / `xtc_tnt_spawn_on` |
| one-shot timers delivering a tagged message | `xtc_tnt_register_timer` |
| thread-per-core, shared-nothing shards | one shard proc per `xtc_exec` loop |

## What is honestly NOT done here (the asterisks)

1. **DST covers the actor core, not the I/O.** libxtc's
   `test/sim/test_sim_tnt.c` runs two shards under the deterministic
   simulator (spawn, mailboxes, drop-on-full, stale handles,
   cross-shard sends, timers, replay).  The socket couriers need a real
   kernel, so this echo server's I/O path is not simulated.

2. **No-malloc-after-boot is DISCIPLINARY, not structural.** Arenas
   (slot metadata + the Isolate-struct store) are carved from boot-time
   `xtc_slab` caches, and message envelopes are reused from a per-shard
   free list.  But a send allocates an envelope when that list is
   empty, every recv allocates its buffer, and couriers and timers are
   spawned with allocation, so "after boot, malloc is never called" is
   not enforced.

3. **Cross-shard messaging is per-pair rings.** A send to another
   shard goes into a single-producer ring for that (source, target)
   pair, and the target moves it into the Isolate's mailbox at the top
   of its next tick; a sender that is not a shard uses
   `xtc_tnt_send_from` and a shared ring.  All shards share one
   address space; there is no cross-process transport.

4. **Supervision is Level-1 only.** A handler returning `XTC_TNT_CRASH`
   or `XTC_TNT_DONE` tears down its own arena slot (bumping the
   generation).  A real SIGSEGV would unwind the shard fiber (Tina's
   Level-2 shard rebuild) -- libxtc has the trap
   (`xtc_proc_recovery_arm`) for it, but wiring shard rebuild + restart
   strategies is follow-on work.

## Build and run

tnt ships inside libxtc, so building the library builds tnt:

```sh
cd ../../build_unix && make            # builds libxtc.a (incl. tnt)
make tnt_echo                          # builds the echo demo
make test_tnt                          # builds the in-process test
make check                             # runs the in-process tnt tests
                                       # (not the TCP echo test, which
                                       # binds a fixed port)
```

Or build just the demo from this directory against an existing build:

```sh
make              # build tnt_echo, linking ../../build_unix/libxtc.a
make echo-test    # run the end-to-end TCP echo round-trip test
make clean
```

The in-process test (`test/tnt/test_tnt.c`) proves, on the real shard
scheduler: spawn into a typed arena, the dispatch loop, WAIT_MESSAGE /
YIELD / DONE transitions, drop-on-full bounded mailboxes with sender
feedback, generational-handle staleness after teardown, and one-shot
timers.

The echo test (`test/tnt/test_tnt_echo.sh`) starts `tnt_echo` on a port,
opens TCP connections (single round-trips plus 8 concurrent), and
verifies each payload is echoed back byte-for-byte across two shards.

### Running the echo server by hand

```sh
./tnt_echo 7777 2        # port 7777, 2 shards (or -p 7777 -s 2)
# in another shell:
printf 'hello' | nc 127.0.0.1 7777
```

## Public API

The entire surface is in `<xtc_tnt.h>`; see `man xtc_tnt`:

- Types: `xtc_tnt_handle_t`, `xtc_tnt_transition_t`,
  `xtc_tnt_message_t`, `xtc_tnt_type_t`, `xtc_tnt_spec_t`.
- Boot: `xtc_tnt_run`, `xtc_tnt_stop`, `xtc_tnt_spawn_on`,
  `xtc_tnt_send_from` (a send from outside a handler).
- Ambient (handler-only): `xtc_tnt_send`, `xtc_tnt_spawn`,
  `xtc_tnt_submit_recv`, `xtc_tnt_io_send`, `xtc_tnt_io_send_owned`,
  `xtc_tnt_recv_take`, `xtc_tnt_submit_close`,
  `xtc_tnt_register_timer`, `xtc_tnt_self`, `xtc_tnt_shard_id`,
  `xtc_tnt_scratch_arena`.
- Helpers: `xtc_tnt_self_as`, `xtc_tnt_payload_as`,
  `xtc_tnt_transition_to_crash`, the `xtc_tnt_handle_*` accessors.

## Files

| File | What |
|---|---|
| `echo.c` | the canonical TCP echo server (this example) |
| `Makefile` | links `../../build_unix/libxtc.a` |

The layer itself lives in the library: `src/orc/tnt.c`,
`src/inc/xtc_tnt.h`, `man/man3/xtc_tnt.3`; its tests live in
`test/tnt/` (`test_tnt.c`, `test_tnt_xshard.c`, `test_tnt_zcopy.c`,
`test_tnt_timer_stop.c`, `test_tnt_echo.sh`) and `test/sim/test_sim_tnt.c`.
