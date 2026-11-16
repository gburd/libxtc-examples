# 20_jobq -- a job queue, tested by deterministic simulation

`jobq.c` is a small job queue with leases, retries and fencing tokens.
`dst.c` is its deterministic simulation test (DST).  It runs the same
service code under libxtc's simulator for hundreds of seeds, with
message latency, network partitions, crashes and the service's own
pessimal paths, and checks the service's invariants after every run.
This example shows how to test your own program the way libxtc tests
itself, using only the installed `xtc_sim.h`.

```sh
# a native libxtc build and a simulation build, side by side
mkdir build_unix build_sim
(cd build_unix && ../dist/configure && make)
(cd build_sim && ../dist/configure --with-io-backend=sim && make)

cd examples/20_jobq
make XTC_BUILD=../../build_unix XTC_SIM_BUILD=../../build_sim check
./jobq_dst -v --replay 17       # one seed, with its trace
make XTC_SIM_BUILD=../../build_sim sweep SEEDS=20000
make XTC_SIM_BUILD=../../build_sim check-plant
```

`make check` runs `./jobq` once on real threads, then `./jobq_dst 0 300`:
seeds 1 to 300, each run twice, in about a second.  An installed
simulation library is `libxtc-sim.a`, and `pkg-config --static --libs
xtc-sim` gives its link line.  In a libxtc build tree, `make check` runs
both halves (`check-jobq`; a native build makes its own simulation
library in `jobq-sim/` the first time), and `make check-jobq-sweep
JOBQ_SEEDS=100000` runs a longer sweep.  20,000 seeds (four shards of
5,000 from bases 0, 5000, 10000 and 15000) pass, with 87,000 injected
crashes and 32,000 network cuts.

## What deterministic simulation testing is

A concurrent program fails in the interleavings nobody tried.  A stress
test tries the interleavings your machine happens to produce.  It cannot
replay the one that failed, and it does not go looking for bad ones.

A deterministic simulation takes the program's sources of
nondeterminism away from the machine and gives them to a seed.  Every
loop runs on one thread, and the seed decides which loop takes the next
step.  The clock is virtual: a `xtc_proc_sleep(5 ms)` takes no real time,
and the clock jumps to the next timer when nothing can run.  Every
injected fault (a delayed message, a dropped one, a killed process, a
slow disk) is a draw from the seed too.  One seed is one exact run.  A
seed that fails fails the same way every time, on any machine, so you
can rerun it, add a print, and watch it fail again.

Because a run takes no real time, a seed costs milliseconds: this
harness runs 300 seeds of a four-loop service, each twice, with 1,401
injected crashes and 458 network cuts per pass, in about a second.  Hours of simulated failure fit in a
`make check`.  FoundationDB built its database this way; TigerBeetle
built its ledger this way.  libxtc runs its own test suite this way
(`make check-dst`), and `xtc_sim.h` lets you do it to your program.

## The service

A producer submits jobs to a queue.  Workers lease a job, do the work,
write the result to an object store, and report the job complete.  The
queue grants each job to one worker at a time, for a limited time (a
lease).  A worker that crashes or stalls loses its lease, and the job is
granted again.  That is what makes a job queue survive worker failure.
It is also what makes it hard to get right: a job can run twice, and a
worker that stalled past its lease can wake up and write its result after
the job's next holder already wrote.

The defence is a fencing token.  Every lease carries a number larger
than any lease granted before it, and the store refuses a write whose
token is lower than one it has already accepted for that job.  A stale
holder's write is then refused, however late it arrives.  Martin
Kleppmann's [How to do distributed locking](https://martin.kleppmann.com/2016/02/08/how-to-do-distributed-locking.html)
explains why a lease without a fence is not safe.

Four loops, all messages:

| loop | procs |
|---|---|
| 0 | the queue, under a supervisor |
| 1 | the object store, the producer (and, under DST, the nemesis) |
| 2 | workers 0 and 1, under a supervisor |
| 3 | workers 2 and 3, under a supervisor |

The queue keeps a journal.  In this example the journal is an in-memory
record standing in for a file: a journal write is a sleep of
`journal_ns` (the fsync), then the update, so a queue killed during
the sleep never made the update.  The journal holds the epoch, each
job's state, and the token that completed it.  Leases are not journaled.
A restarted queue makes its new epoch durable first, forgets every lease,
and builds its tokens as `epoch << 32 | n`, so every token it grants is
above every token any earlier incarnation granted.

Clients find the queue and the store by name in an `xtc_reg` registry,
because the queue's pid changes when it restarts.  Every request carries
a request id and the client's pid, and a client that hears nothing for
10 ms retries.  Every request is idempotent, so a retry after a lost
reply is safe: a second submit of a known job is acknowledged again, and
a repeated completion gets the same answer as the first.

## The harness

`dst.c` is about 450 lines, and the service does not know it exists.
For each seed it does the following.

1. **Draws the run's faults from the seed**, swarm style: each fault
   class is on or off per seed, so some runs see one class alone and
   others see them all at once.  The classes are cross-loop message
   latency (`xtc_sim_net_latency`), one-slot delivery reordering
   (`xtc_sim_swizzle_enable`), the pessimal scheduler that starves a loop
   (`xtc_sim_sched_pessimal`), clock jitter (`xtc_sim_clock_skew`), and
   buggify (`xtc_sim_buggify_enable`).
2. **Spawns a nemesis** proc next to the service.  At seeded times it
   kills the queue, kills a worker (`xtc_exit_pid`; the supervisors
   restart them), or cuts a loop off the network for a while
   (`xtc_sim_partition_isolate`), then heals everything and leaves.
3. **Runs** with `xtc_sim_exec_run(exec, seed, budget)`.  The run ends at
   quiescence: every proc has exited and nothing is pending.
4. **Checks the invariants** at quiescence, through
   `xtc_sim_set_consistency_check`:
   * ACKED-LOST: a job the queue acknowledged is never lost, whatever
     crashed.
   * FENCE: a write under an older lease never lands after a write under
     a newer one.
   * DOUBLE-EXEC: two workers never both run a job under the same lease.
   * RESULT: the stored result of a completed job is the completing
     lease's write.
   * LIVENESS: every job is done.  With the nemesis gone and the network
     healed, a correct queue always finishes.
5. **Runs the seed again** and compares: the service's trace hash (every
   state change with its virtual time, `jq_trace`), the simulator's
   `xtc_sim_state_hash`, and the counters.  A difference means something
   in the run did not come from the seed, and the seed is not a
   reproducer.  libxtc's determinism guard catches a real clock, the
   environment or a thread id read on a simulated path.  The replay
   check catches the rest, such as an uninitialised variable or a
   pointer used as a hash key.

A failure prints the seed, the run's faults, and the command that
replays it:

```
FAIL seed 3: FENCE: job 9: a write under lease 10000000c landed after one under the newer lease 200000001
  faults: latency=1 swizzle=0 pessimal=0 jitter=0 buggify=1 kills=1 cuts=0
  replay: ./jobq_dst -v --replay 3
```

`-v` prints the run's trace, one line per state change with its virtual
time, so you can read the failure as it happened:

```
   111.286 ms  queue.grant        9  10000000c
   115.483 ms  nemesis.kill_q     0  0
   116.483 ms  queue.epoch        0  2
   118.886 ms  queue.grant        9  200000001
   125.240 ms  store.write        9  200000001
   127.159 ms  store.write        9  10000000c
```

The queue leased job 9 as `1:0xc` (epoch 1, grant 12).  The nemesis
killed the queue, which restarted as epoch 2, forgot the lease, and
leased the job again as `2:0x1`.  The new holder wrote first, and the
old holder's write landed two milliseconds later.  The fence would have
refused it; this build has the fence planted out.

A passing sweep reports what it covered, because a green run that never
reached the dangerous states proves little:

```
OK: 300 seeds, each run twice and replayed identically
  nemesis: 674 queue kills, 727 worker kills, 458 network cuts
  seeds where the queue restarted 212, a lease was granted again 257, the store fenced a stale write 166
  buggify jq.worker.pause_before_write reached in   452 runs, on in   174
  ...
```

A site that is never on in a sweep is a pessimal path no seed tested.

### Buggify: the service's own fault points

Library faults do not reach the decisions that make a job queue subtle.
`jobq.c` marks those decisions with `XTC_SIM_BUGGIFY("name")`.  Under the
simulator a seeded coin, flipped once per run per site, makes a site
take its pessimal path for the whole run.  In a normal build each
mark costs one load of a flag that is never set.

| site | the pessimal path |
|---|---|
| `jq.worker.pause_before_write` | a worker checks its lease, then stalls for three lease lengths before it writes (a long GC pause, a stalled VM) |
| `jq.worker.abandon` | a worker drops a job without telling anyone |
| `jq.worker.dup_complete` | a worker reports a completion twice |
| `jq.queue.short_lease` | leases an eighth as long as configured, shorter than the work |
| `jq.queue.slow_journal` | journal writes eight times slower, so kills land inside them |
| `jq.producer.dup_submit` | the producer submits every job twice |

Each is a legal behaviour a real deployment produces sooner or later.
The pause before the write and the queue's restarts are what reach the
fence.  Without them a stale write needs a lease to expire at the exact
instant a worker is between its check and its write, which a random
schedule seldom hits.  With them, the store refuses a stale write in 166
of the 300 seeds of `make check`.

### Planted bugs: does the harness catch anything?

A green sweep proves nothing until you know the harness fails when the
code is wrong.  `jobq.c` carries three deliberate bugs, each behind
`XTC_DST_BUG(n)`, which is 1 only in a build made with
`-DXTC_DST_INJECT_BUG=n` (here `make PLANT=n`).  `make check-plant`
builds each and requires the 300-seed run to fail:

| plant | the bug | caught at | as |
|---|---|---|---|
| 1 | the store accepts a write without checking the fence | seed 3 | FENCE |
| 2 | the queue acknowledges a submit before its journal write | seed 7 | ACKED-LOST |
| 3 | a restarted queue reuses its old epoch, so tokens repeat | seed 3 | DOUBLE-EXEC |

The number to watch is how many seeds a bug survives.  A bug the sweep
catches in a handful of seeds is a bug you will not ship.  A planted
bug that survives the whole sweep is a hole in the harness: a fault that
is not injected, or an invariant that is not checked.

## What DST found while writing this example

The first version of the shutdown asked the store to stop with a
message, looked up by name.  Seed 17 deadlocked: the run ended with the
store alive and parked.  `-v` showed both stop requests timing out.  The
cause was a buggify site inside libxtc, `reg.whereis.transient_miss`,
which makes a registry lookup miss now and then, as a lookup that races
an unregister can.  Both lookups missed, and the store was never told.
The fix kills the store by pid.  That is a real bug class (shutdown
depending on a message that can be lost), found in seventeen seeds,
with a replayable trace.

## Adopting DST for your own program

1. **Keep the service free of the harness.**  The service is a function
   that spawns procs on an `xtc_exec` (`jq_start`).  `main.c` calls it
   and `xtc_exec_run`; `dst.c` calls it and `xtc_sim_exec_run`.  Nothing
   in `jobq.c` tests whether it is simulated, except to skip recording
   the trace.
2. **Use the clocks and the randomness the simulator controls.**  Time
   comes from `xtc_clock_mono` and `xtc_proc_sleep`; never
   `clock_gettime` or `xtc_clock_real` on a simulated path.  Random
   choices come from `xtc_sim_rand` (the seed's application stream under
   the simulator, `xtc_rand_u64` otherwise).
3. **Make every wait bounded.**  A proc that waits forever for a message
   that was dropped makes the run end in `XTC_E_DEADLK`, which is a
   correct report of a real hang.  Use timeouts and retries.
4. **Write the invariants down as code** that reads the service's state
   after the run (`jq_check`).  Check safety (nothing acknowledged was
   lost, nothing ran twice) and liveness (everything finished).
5. **Mark your decision points** with `XTC_SIM_BUGGIFY`.
6. **Prove the replay** by running each seed twice and comparing a
   trace hash you build from your own state changes.
7. **Plant a bug** for each invariant and prove the sweep catches it.
8. **Keep the failing seeds.**  A seed that failed once is a regression
   test.  Put it in your `make check` next to the random sweep.

## Design choices, and the alternatives not taken

* **Messages between loops, not sockets.**  The simulator models
  cross-loop `xtc_send`: latency, reordering and partitions.  It does
  not model real sockets.  Under a simulated run a socket never becomes
  ready, because the simulator has no kernel poller.  A service that
  talks over `xtc_net` therefore needs a transport seam to be simulated
  (messages under the simulator, sockets in production).  This example
  makes the loops the network instead, so the fault model and the
  service meet at the same seam.
* **Supervisors next to their children.**  Each supervisor runs on its
  children's loop.  The simulator's partition drops every message on a
  cut edge, including the DOWN a supervisor waits for.  A supervisor on
  another loop never restarts a child that died during a partition, and
  the run hangs.  Real distributed Erlang reports such a death as a
  DOWN with reason `noconnection`.
* **A modelled journal, not a file.**  The journal is memory plus a
  sleep for the fsync.  The crash semantics (killed before the update,
  or after) are what the invariants need, and the example stays
  readable.  `xtc_sim_io_wb_enable` and the AIO fault knobs model a real
  file's write-back cache, torn writes and fsync errors; libxtc's own
  `test_sim_wal_kill` shows them on a real log.
* **Fencing, not exactly-once.**  The queue promises at-least-once
  execution with fenced effects.  Exactly-once execution needs the
  worker's effect and the completion to commit together, which a store
  outside the queue cannot do.  The fence is the standard answer, and
  the one the invariants check.
* **The oracle reads the state at the end, not at every step.**  A
  check at quiescence sees the store's whole write log and the journal,
  which is where the safety properties live.  A per-step check would
  catch a violation earlier in the trace but would have to run inside
  the service.
* **Swarm fault selection.**  A fault mix fixed for every run would
  hide bugs that one fault class masks in another.  Turning each class
  on or off per seed tests the combinations as well as the classes.
  The idea is from swarm testing (Groce et al., ISSTA 2012).

## Files

| file | what |
|---|---|
| `jobq.h` | the service's interface: start, finish, check, trace |
| `jobq.c` | the service: queue, store, workers, producer, invariants |
| `main.c` | runs the service on real threads |
| `dst.c` | the DST harness |
