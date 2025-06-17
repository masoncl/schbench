
# Motivations

schbench is meant to reproduce the scheduler characteristics of our production
web workload with a relatively simple benchmark.  It's really targeting three
things:

- Saturate all the CPUs on the system.  Leaving idle CPUs behind will result in
  lower RPS.
- Long timeslices.  Involuntary context switches will result in lower RPS.
- Low scheduling delays.  Higher wakeup latencies will result in lower RPS.

# Methodology

schbench uses messaging threads and worker threads.  Workers perform an
artificial request comprised of two usleeps (simulating network/disk/locking)
and some matrix math.  Messaging threads just queue up the work and wait for
results.

Results are recorded for three primary metrics:

- Wakeup latency: messaging threads record the time a worker is posted, and
  workers compare this with the time when they start running.
- Request latency: time required to complete our fake request.
- Requests per second: total number of requests all the threads are able to
  complete.

## Penalizing preemption

The workload we're simulating has a complex set of interactions between CPU,
memory, and network/disk IO.  Instead of trying to reproduce all of that,
schbench just makes preemption expensive.   A per-cpu spinlock that is taken
while performing our matrix math, and is released whenever the math is done.

If the thread is moved to another CPU in the middle of the critical section,
whoever tries to take the per-cpu lock next will have to wait until
calculations are finished.

This isn't a great model for multi-threaded programming, and it can be turned
off with (`-L / --no-locking`), but it seems to be the most accurate way to match
what we're seeing in the real world.

## Calibration

If the matrix math portion of a request is longer than our timeslice, the
resulting contention will impact performance.  Two different command line
options control how long the matrix math will take:

`-F, --cache-footprint <KB>`: cache footprint (def: `256`)
`-n, --operations <N>`: think time operations to perform (def: `5`)

Another important option is:

`-C, --calibrate`: run our work loop and report on timing

In this mode, no spinlocks are taken, and the request latency times are based
entirely on the matrix math (no usleeps are counted).  You can use these three
options to match the work to your desired timeslice.  The goal is to get the
`p99` of your request latency in calibration mode just under the desired
timeslice.

# Example runs

## Calibration run

This lands at just under 20ms for the matrix math done in our request

```bash
$ ./schbench -F 256 -n 5 --calibrate -r 10
setting worker threads to 52
...
Wakeup Latencies percentiles (usec) runtime 10 (s) (30212 total samples)
          50.0th: 7          (7806 samples)
          90.0th: 18         (8649 samples)
        * 99.0th: 175        (2590 samples)
          99.9th: 909        (271 samples)
          min=1, max=10224
Request Latencies percentiles (usec) runtime 10 (s) (30246 total samples)
          50.0th: 16864      (10050 samples)
          90.0th: 16992      (12843 samples)
        * 99.0th: 17248      (616 samples)
          99.9th: 19872      (261 samples)
          min=9243, max=27964
RPS percentiles (requests) runtime 10 (s) (1 total samples)
          20.0th: 3020       (1 samples)
        * 50.0th: 3020       (0 samples)
          90.0th: 3020       (0 samples)
          min=3017, max=3017
average rps: 3024.60
```

## Max loading all the CPUs

This does a 90 second run using the parameters above.  The request latencies
are a little higher because they do include the time spent on usleeps when not
in calibration mode.

```bash
$ ./schbench -F 256 -n 5 -r 90
...
Wakeup Latencies percentiles (usec) runtime 90 (s) (255474 total samples)
          50.0th: 7          (66640 samples)
          90.0th: 20         (68973 samples)
        * 99.0th: 272        (21770 samples)
          99.9th: 1266       (2301 samples)
          min=1, max=10481
Request Latencies percentiles (usec) runtime 90 (s) (255755 total samples)
          50.0th: 17120      (85972 samples)
          90.0th: 17568      (65273 samples)
        * 99.0th: 19616      (22817 samples)
          99.9th: 26464      (2249 samples)
          min=10794, max=48606
RPS percentiles (requests) runtime 90 (s) (9 total samples)
          20.0th: 2996       (3 samples)
        * 50.0th: 3004       (2 samples)
          90.0th: 3020       (4 samples)
          min=2982, max=3021
average rps: 3009.10
```

## Pipe mode

Pipe mode uses futexes and shared memory instead of pipes, but the underlying
idea is similar to perf bench.  Wakeup latencies are recorded.

```bash
$ ./schbench -p 65536 -t 4
Wakeup Latencies percentiles (usec) runtime 30 (s) (2631154 total samples)
          20.0th: 6          (807309 samples)
          50.0th: 7          (1729909 samples)
          90.0th: 7          (0 samples)
        * 99.0th: 8          (71575 samples)
          99.9th: 14         (20272 samples)
          min=1, max=562
avg worker transfer: 21924.84 ops/sec 1.34GB/s
```

# All the options

`-C, --calibrate`: run our work loop and report on timing
Documented above

`-L, --no-locking`: don't spinlock during CPU work (def: `locking on`)
Use this if you don't want to penalize preemption.

`-m, --message-threads <N>`: number of message threads (def: `1`)
One message thread per NUMA node seems best.

`-t, --threads <N>`: worker threads per message thread (def: `num_cpus`)
These do all the actual work, but you shouldn't need more than num_cpus.

`-r, --runtime <SECONDS>`: How long to run before exiting (def: `30`)

`-F, --cache-footprint <KB>`: cache footprint (def: `256`)
The size of our matrix for worker thread math.

`-n, --operations <N>`: think time operations to perform (def: `5`)
The number of times we'll loop on the matrix math in each request.

`-s, --sleep-usec <USEC>`: time to sleep during each requests, in usec (def: `100`)

`-A, --auto-rps <PERCENT>`: grow RPS until cpu utilization hits target (def: `none`)
Instead of trying to fully saturate the system, target a specific CPU utilization percentage.

`-p, --pipe <BYTES>`: transfer size bytes to simulate a pipe test (def: `0`)
perf pipe test is bottlenecked on pipes, this aims to move the bottleneck to the scheduler instead.

`-R, --rps <COUNT>`: requests per second mode (def: `0`)
Instead of trying to fully saturate the system, target a specific number of requests per second.

`-w, --warmuptime <SECONDS>`: how long to warmup before resettings stats (def: `5`)
Once the workload is stabilized, we zero all the stats to get more consistent numbers.

`-i, --intervaltime <SECONDS>`: interval for printing latencies (def: `10`)
Print a report about our latencies and RPS ever N seconds.

`-z, --zerotime <SECONDS>`: interval for zeroing latencies (def: `never`)
Zero all of our stats on a regular basis.

`-M, --message-cpus <LIST>`: list of cpus (a-n,m-z) the message threads are allowed to use, or "`auto`"

`-W, --worker-cpus <LIST>`: list of cpus (a-n,m-z) the worker threads are allowed to use, or "`auto`"

Each message thread is pinned to a single CPU, round robin style through the set of
supplied CPUS.

Each worker thread is pinned to the entire set of supplied CPUs.  These two
sets are allowed to overlap.

If auto pinning mode is used, each message thread is pinned to a single CPU,
starting with CPU 0.  Worker threads are pinned to all of the remaining CPUs.
