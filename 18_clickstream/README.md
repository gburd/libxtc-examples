# 18_clickstream -- web analytics: a stream pipeline, a Bloom filter, HyperLogLog

`clickstream.c` reads the access logs of four front-end servers, one proc
per loop of a four-loop `xtc_exec`, and answers two questions: how many
distinct users visited, and how many of them checked out.  Each log runs
through a lazy `xtc_stream` pipeline; a Bloom filter (`xtc_bloom`) drops
lines the log shipper delivered twice; and HyperLogLog sketches
(`xtc_hll`) count distinct users, one per front end, merged at the end.

```sh
make 18_clickstream && ./18_clickstream    # from a libxtc build tree
make XTC_BUILD=../../build_unix test       # from this directory
```

The logs are synthetic: 50,000 lines per front end from 20,000 user ids,
a fifth of the traffic from 500 heavy users.  About 12% of the lines are
a crawler or the load balancer's health checks, 0.5% are truncated in
transit, and the shipper delivers 3% of the lines twice.  The generator
also keeps an exact oracle (bitmaps of the users, counts of each kind of
line), so the program can show the sketches' error:

```
distinct users: estimate 20111, exact 19980 (+0.66%)
  (adding the 4 per-front-end estimates gives 66121: a user seen by several front ends counts several times)
distinct users who checked out: estimate 2548, exact 2553 (-0.20%)
conversion: 12.7% estimated, 12.8% exact
Bloom filter: 72 of 169549 new lines lost to false positives (0.042%, sized for 1%)
```

The program checks that every redelivered line was dropped, that every new
line was either counted or lost to a Bloom false positive, that the
false-positive rate is within the configured 1%, and that both estimates
are within four standard errors of the exact counts.  `expected.txt` is the
full output; `make check` in the libxtc build tree diffs against it.

## What it shows

**A pipeline as composed streams.**  Each front end's pipeline is

```
xtc_stream_create(log_next)          one log line per pull
  -> xtc_stream_map(parse)           line -> struct hit
  -> xtc_stream_filter(well_formed)  drop truncated lines
  -> xtc_stream_filter(human)        drop crawler and health checks
  -> xtc_stream_filter(first_delivery)  drop redeliveries (Bloom)
  -> xtc_stream_for_each(aggregate)  count, add to the sketches
```

Nothing runs until `xtc_stream_for_each` pulls, and each pull carries one
line through every stage, so a shard holds one line at a time however long
its log.  Each stage is a small function with one job, and the stages can
be reordered or tested alone.  `xtc_stream_destroy` on the outermost
stream frees the whole chain.

**Deduplication in fixed memory.**  The shipper resends a line it got no
acknowledgement for, so the pipeline sees some request ids twice.  An exact
set of the ids seen grows with the log.  The Bloom filter is sized once,
for the lines one front end logs at the target rate (1%), and stays that
size.  It never lets a repeat through; the cost is that a new line can
test "seen" and be dropped.  Here that is 72 of 169,549 page views, which
an analytics report can carry and a billing system could not.

**Distinct counts that merge.**  A distinct count does not add up across
front ends, because one user hits several of them: the four per-front-end
estimates sum to 66,121 for 19,980 real users.  An exact answer needs one
set of user ids shared by every shard, with the locking and the memory that
implies.  Each shard keeps its own 4 KiB HyperLogLog sketch instead, with
no sharing while the shards run, and main merges them with `xtc_hll_merge`,
which is exact: the merged sketch is the sketch of the union.  This is the
pattern `xtc_prob(3)` recommends, since the structures take no locks.

## Design choices

* **One shard per loop, nothing shared.**  Each shard owns its generator,
  its Bloom filter and its sketches; the shards touch nothing in common
  until main merges them after `xtc_exec_run` returns.  That is why the
  probabilistic structures need no locks.
* **Deduplicate per shard.**  A redelivery goes back to the front end that
  sent it, so each shard sees its own repeats.  Request ids carry the front
  end's number, so they are unique across the fleet anyway.
* **The source tells the truth to the oracle, not to the pipeline.**  The
  generator records whether each line is a repeat; only the counters read
  that, to tell the Bloom filter's two kinds of error apart.  The pipeline
  itself decides from the request id alone.
* **Deterministic.**  The generator is seeded per shard, the hashes are
  fixed, and a merge does not depend on order, so the output is identical
  on every run and every host, and a test can diff it.
* **Precision 12.**  4,096 registers, 4 KiB per sketch, a standard error
  of 1.6%.  Precision 14 would cut the error to 0.8% for 16 KiB.

## Alternatives not chosen

* **An exact hash set of user ids.**  Correct, and fine at 20,000 users.
  At a hundred million it is gigabytes per shard, and the shards would have
  to share it or ship their sets to a merger.
* **Deduplicating with an exact set of recent ids** (a time window).  It
  bounds memory by the window, not the log, and is the right call when a
  lost line costs money.  The Bloom filter needs no window: it is sized
  for the day.
* **A demand channel between a reader proc and the pipeline**
  (`xtc_stream_from_demand`).  Useful when lines arrive from a socket on
  another fiber.  Here the source is local, so the pipeline pulls from it
  directly.
* **A materialized pipeline** (parse the whole log into an array, then
  filter, then count).  Simpler to step through in a debugger; memory grows
  with the log, and nothing is counted until everything is parsed.
