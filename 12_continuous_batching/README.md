# Continuous batching

A public-API numerical example with two batch groups and an unrelated
heartbeat process on one loop. Each group has its own scheduling class.
Read [main.c](main.c) for the complete runnable example and
[the guide](../../docs/examples/continuous-batching.md) for the design.

## Build and run

Configure libxtc with `--enable-sched-policy`, then build `libxtc.a` in
that configured directory. From this directory, run
`make XTC_BUILD=/absolute/path/to/configured-build test`.
`XTC_BUILD` defaults to `../../build_unix`; the Makefile obtains link
libraries and linker flags from that build's Makefile. The library and
example must use compatible compiler/sanitizer flags.

The binary accepts:

- `--mode=static|continuous`: wait for the original cohort to finish, or
  refill a free active slot between iterations. Default: `continuous`.
- `--policy=default|continuous`: ordinary class scheduling, or the built-in
  batch-lane preference with a maximum bypass budget of 3. Default: `default`.
- `--mode=restart`: a supervised engine owns its group and is killed
  mid-batch; the restarted engine recreates the group (see below).
- `--self-test`: run and check all four combinations and the restart
  mode, regardless of the selected mode and policy.
- `--help`: print usage without initializing a loop.

For example, run `./continuous_batching --mode=static --policy=default`,
then `./continuous_batching --mode=continuous --policy=continuous`.
`make test` also checks help and rejection of invalid mode/policy values.

## What to look for

Both modes perform **1120 component updates** using the same vector step.
For each group, a long request takes 64 rounds, a short request takes 4,
and the late request takes 2. Each round updates 8 unsigned 64-bit
components. The fourth request is cancelled before any arithmetic.
The step prepares coefficients once for the whole vector and applies
an affine recurrence to each member. Unsigned wraparound is intentional;
an independent component-major scalar reference checks every result.

The initial three submissions exhaust each group's capacity. An attempt
to submit the late request returns `XTC_E_AGAIN`; the caller retains it
and retries after capacity becomes available. After two rounds the long
request reports `BLOCKED`, representing a missing dependency. The owner
process observes that state between callbacks, retries the late request
(in continuous mode), and resumes the long request. Static mode holds
the retry until the original cohort finishes instead.

The continuous run reports `late=3 long=4`; static reports `late=4 long=3`.
These are per-group terminal ordinals, including cancellation. Completion
order, exact values, one terminal callback per accepted request, resource
release, cancellation, retry, and heartbeat progress are assertions, not
timing thresholds. A cooperative watchdog catches lack of progress when
the heartbeat can still run; an external timeout is appropriate for CI.

This is an admission-order demonstration, not a throughput benchmark.
There are no sleeps, wall-clock measurements, fake costs, GPU kernels,
or claims that the scheduling policy accelerates arithmetic. Policy and
batch membership are independent: continuous refill also works under the
default policy.

## A group owned by a supervised engine

`--mode=restart` is the restartable-scheduler shape (libxtc 2.2). The
engine is a TRANSIENT child of a supervisor and creates its group itself,
on the running loop, then submits every request that has not finished. A
watcher kills the first incarnation after a few vectors. The group
belonged to the dead engine, so libxtc closes it with `XTC_BATCH_CANCEL`
and its unfinished requests complete with `XTC_E_ABORTED`; the example
starts those over. The supervisor restarts the engine, and the new
incarnation destroys the old group (retrying while its driver is still
ending), creates a new one on the same loop and resubmits. It prints
`mode=restart: incarnations=2 aborted=3 terminals=4 OK`, and the scalar
reference checks every request's arithmetic from its restart.

## Ownership

In the two batching modes, classes and groups are created before the
loop's first run. Setup mutations
are externally serialized; later submit/resume/close operations run in
the owner process on that same loop. The callbacks only compute results
or record completion. They do not yield, mutate groups, or call across
threads. Request storage remains alive through completion and teardown.

The owner closes both groups with `XTC_BATCH_DRAIN`; `xtc_loop_run` returns
after the drivers and application processes exit. Only then does the
example inspect final stats, destroy both groups, and finalize the loop.
Stopping a loop would not substitute for draining it. A disabled-feature
build fails with a diagnostic requesting `--enable-sched-policy`.
