# 20_jobq -- a job queue, tested by deterministic simulation

`jobq.c` is a small job queue with leases, retries and fencing tokens.
`dst.c` is its deterministic simulation test (DST): it runs the same
service code under libxtc's simulator for hundreds of seeds, with
latency, partitions, crashes and the service's own pessimal paths, and
checks the service's invariants after every run.  This example shows how
to test your own program the way libxtc tests itself, using only the
installed `xtc_sim.h`.

```sh
make XTC_BUILD=../../build_unix XTC_SIM_BUILD=../../build_sim check
./jobq_dst --replay 17 -v      # one seed, with its trace
make sweep SEEDS=20000         # a longer sweep
make check-plant               # every planted bug must be caught
```

`XTC_SIM_BUILD` is a libxtc configured `--with-io-backend=sim`.  An
installed one is `libxtc-sim.a`, found by `pkg-config --static --libs
xtc-sim`.

(Full design notes follow in the next revision of this file.)
