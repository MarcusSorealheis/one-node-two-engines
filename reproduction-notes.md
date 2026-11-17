# Reproduction: one node, two engines

Every number in the article's terminal block comes from session 3 below. `contend.c` (355 lines), `psiwatch.c` and `run.sh` are in this repository, and the raw logs are in [`results/`](results/). It is a model of two I/O patterns, not mongod, mongot or Postgres.

## Run it

```bash
gcc -O2 -pthread -o contend contend.c && gcc -O2 -o psiwatch psiwatch.c
mkdir -p data/db data/seg
./contend mkfile data/index 3072 && ./contend mkfile data/db/collection 3072
sudo bash run.sh    # about 18 minutes: search alone, db alone, one node
```

## What each process models

| Process | Stands in for | What it does |
| --- | --- | --- |
| `contend search` | A Lucene-style engine: mmap'd index, segment flushes, merges | Maps a 3 GiB index read-only, preloads it and sets `MADV_RANDOM`. Runs 200 queries/s, each touching 128 uniformly random 4 KiB pages. An indexer thread writes the incoming document stream again as 32 MiB segments, fsyncs each, and merges four segments of a tier into one at 32 MB/s (32 to 128 to 512 MiB). A new 512 MiB segment supersedes the previous one, as in an update workload. |
| `contend db` | A transactional engine: private cache, WAL, B-tree file, checkpoints | Fills a 1.5 GiB cache of anonymous memory. Runs 250 transactions/s: four 8 KiB `pread`s from a 3 GiB collection file, a 32 KiB WAL append with `fdatasync`, then four 8 KiB in-place `pwrite`s to the collection. A checkpointer calls `sync_file_range` every second and `fdatasync` every 10 s. |

Both loops are open: latency runs from each request's scheduled arrival, so time spent queued counts.

## Environment

- An ephemeral 2-vCPU Firecracker VM, a cloud sandbox: Linux 6.18.44, ext4 on virtio-blk, no swap, `vm.dirty_ratio=20`, classic active/inactive LRU.
- Disk: 686 MB/s sequential write with `fdatasync` and 3.2 GB/s cold sequential read (`dd`, 1 GiB), measured before the first session.
- All processes ran in one cgroup v1 memory cgroup limited to 6 GiB. That limit is the "node"; "alone" means a process had the 6 GiB to itself.
- Sizing: index at 50% of RAM and database cache at 25%, as in the co-located sizing section of [MongoDB's deployment docs](https://www.mongodb.com/docs/vector-search/deployment/deployment-options/) (M30: 8 GB total, 4 GB for the index, 25% reserved for MongoDB).
- Each scenario: drop caches, start, 150 s of load as ramp, then 180 s measured.

## Results by session

Same code and parameters each time. Session 3 is the one in the article because it is the only one with the full PSI capture.

| Session | Search p99, alone | Txn p99, alone | Search p99, one node | Txn p99, one node | Index in RAM, one node (min) | PSI io some: search / db / one node |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 0.48 ms | 5.79 ms | 962.59 ms | 564.62 ms | 68.3% | 3.3% / 12.9% / 31.0% |
| 2 (one node only) | | | 794.33 ms | 458.82 ms | 68.5% | 22.8% |
| 3 (in the article) | 1.08 ms | 59.90 ms | 745.60 ms | 504.55 ms | 70.3% | 3.4% / 17.1% / 27.1% |

The one-node numbers repeat within about 30%. The database-alone tail does not: transaction p99 was 5.79 ms in session 1 and 59.90 ms in session 3, where nearly every 15 s bucket sat between 50 and 66 ms. The sandbox's disk flushes were evidently slower in the later session. So the search comparison is roughly 700 to 2,000 times worse on one node, and the transaction comparison is anywhere from 8 to 100 times worse depending on the baseline. Quote neither without re-running on dedicated hardware.

## Pressure stall information, session 3

The PSI columns are the share of the measured window in which some task (`some`) or every non-idle task (`full`) was stalled on the resource. Each is the change in the `total=` field of that line in `/proc/pressure/io` or `/proc/pressure/memory` between the start and end of the window, divided by the window length, as the kernel's [PSI documentation](https://docs.kernel.org/accounting/psi.html) describes.

| Scenario | io some | io full | memory some | memory full | cpu some |
| --- | --- | --- | --- | --- | --- |
| search alone | 3.4% | 3.3% | 0.0% | 0.0% | 0.4% |
| db alone | 17.1% | 16.7% | 0.0% | 0.0% | 0.5% |
| one node | 27.1% | 26.6% | 0.8% | 0.8% | 1.9% |

- Memory pressure is under 1% on the shared node even though 30% of the index was evicted and a million page touches faulted. The stall time lands under I/O.
- With only a handful of tasks, `full` tracks `some` closely: when the one busy thread waits, every non-idle task is waiting.
- The readings are system-wide. The memory limit here is a cgroup v1 cgroup, which has no per-cgroup pressure files, and the benchmark was the only workload on the VM. On cgroup v2, each container's `io.pressure` and `memory.pressure` show which engine is paying.

PSI triggers. The kernel documentation's two examples are memory `some 150000 1000000` and io `full 50000 1000000`. The sandbox lacks `CAP_SYS_RESOURCE`, so the window had to be a multiple of 2 s; `psiwatch` registered the same rates over 2 s (memory `some 300000 2000000`, io `full 100000 2000000`) and counted events over the 90 windows of each measured run.

| Scenario | Memory trigger fired | I/O trigger fired |
| --- | --- | --- |
| search alone | 0 of 90 | 17 of 90 |
| db alone | 0 of 90 | 70 of 90 |
| one node | 2 of 90 | 69 of 90 |

At that threshold the I/O trigger does not separate the shared node from the database alone: this model's commit and checkpoint fsyncs already stall for more than 100 ms in most 2 s windows. The latency tables separate them; a production alert would need a higher threshold or the `some` average.

`/proc/pressure` at the end of each measured window:

```text
# search alone, end of measured window
/proc/pressure/io:some avg10=1.97 avg60=1.98 avg300=2.63 total=26320361
/proc/pressure/io:full avg10=1.97 avg60=1.98 avg300=2.60 total=25969125
/proc/pressure/memory:some avg10=0.00 avg60=0.00 avg300=0.00 total=49287
/proc/pressure/memory:full avg10=0.00 avg60=0.00 avg300=0.00 total=49029
# db alone, end of measured window
/proc/pressure/io:some avg10=17.05 avg60=16.69 avg300=13.33 total=91902153
/proc/pressure/io:full avg10=16.65 avg60=16.33 avg300=13.09 total=90442598
/proc/pressure/memory:some avg10=0.00 avg60=0.00 avg300=0.00 total=49287
/proc/pressure/memory:full avg10=0.00 avg60=0.00 avg300=0.00 total=49029
# one node, end of measured window
/proc/pressure/io:some avg10=3.74 avg60=18.37 avg300=20.18 total=178045548
/proc/pressure/io:full avg10=3.64 avg60=17.93 avg300=19.75 total=174885062
/proc/pressure/memory:some avg10=0.01 avg60=0.22 avg300=0.17 total=1855227
/proc/pressure/memory:full avg10=0.01 avg60=0.22 avg300=0.16 total=1849500
```

## Raw results

Session 3, `results.log` as written by `run.sh` (also in `results/session-3.results.log`). `refault_file` and `pgsteal_file` are in 4 KiB pages; `trig_*` are trigger counts.

```text
## node: 2 vCPU, 6.0 GiB RAM (memory cgroup limit), Linux 6.18.44, ext4, no swap
## data: 3072 MiB mmap index; 3072 MiB collection + 1536 MiB private db cache
## load: 200 queries/s x 128 random pages; 250 txns/s x (4 reads + 32 KB commit)
##       150 s ramp, then 180 s measured
search alone|search n=36000 p50_us=200 p99_us=1084 p999_us=10144 max_us=28106 majflt=0 resident_min_pct=100.0 resident_end_pct=100.0 flushed_mb=2560 merged_mb=5120 merges=25 chk=0||refault_file=0 pgsteal_file=0 psi_io_some_pct=3.4 psi_io_full_pct=3.3 psi_mem_some_pct=0.0 psi_mem_full_pct=0.0 psi_cpu_some_pct=0.4 trig_mem_some=0 trig_io_full=17
db alone||db n=45000 p50_us=806 p99_us=59897 p999_us=130990 max_us=213859 written_mb=2578|refault_file=0 pgsteal_file=0 psi_io_some_pct=17.1 psi_io_full_pct=16.7 psi_mem_some_pct=0.0 psi_mem_full_pct=0.0 psi_cpu_some_pct=0.5 trig_mem_some=0 trig_io_full=70
one node|search n=36000 p50_us=1919 p99_us=745602 p999_us=1026724 max_us=1074321 majflt=1018417 resident_min_pct=70.3 resident_end_pct=87.6 flushed_mb=2560 merged_mb=5120 merges=25 chk=0|db n=45000 p50_us=811 p99_us=504547 p999_us=943626 max_us=1052883 written_mb=2578|refault_file=1198834 pgsteal_file=1933293 psi_io_some_pct=27.1 psi_io_full_pct=26.6 psi_mem_some_pct=0.8 psi_mem_full_pct=0.8 psi_cpu_some_pct=1.9 trig_mem_some=2 trig_io_full=69
```

Session 1 (three scenarios) and session 2 (one node only), same format. Only the I/O `some` figure was recorded in those runs. The files in `results/` hold these two sessions as an earlier revision of `run.sh` recorded them; see [`results/README.md`](results/README.md).

```text
search alone|search n=36000 p50_us=155 p99_us=476 p999_us=3058 max_us=23941 majflt=0 resident_min_pct=100.0 resident_end_pct=100.0 flushed_mb=2560 merged_mb=5120 merges=25 chk=0||refault_file=0 pgsteal_file=0 psi_io_some_pct=3.3
db alone||db n=45000 p50_us=913 p99_us=5791 p999_us=19221 max_us=73616 written_mb=2578|refault_file=0 pgsteal_file=0 psi_io_some_pct=12.9
one node|search n=36000 p50_us=2162 p99_us=962591 p999_us=1265089 max_us=1295612 majflt=1031338 resident_min_pct=68.3 resident_end_pct=87.3 flushed_mb=2560 merged_mb=5120 merges=25 chk=0|db n=45000 p50_us=904 p99_us=564623 p999_us=849116 max_us=923646 written_mb=2578|refault_file=1209130 pgsteal_file=1950013 psi_io_some_pct=31.0
one node|search n=36000 p50_us=1983 p99_us=794334 p999_us=1192492 max_us=1226669 majflt=1022221 resident_min_pct=68.5 resident_end_pct=87.8 flushed_mb=2560 merged_mb=5120 merges=25 chk=0|db n=45000 p50_us=812 p99_us=458817 p999_us=929685 max_us=1082948 written_mb=2578|refault_file=1203038 pgsteal_file=1951903 psi_io_some_pct=22.8
```

## Latency over time

Session 3, 15 s buckets, milliseconds, built from `results/session-3.timeseries.log`. The measured window is 150 to 330 s.

```text
          search alone      search, one node    txn, db alone      txn, one node     index in RAM
  t (s)   p50      p99      p50       p99       p50     p99        p50      p99       (one node, min)
  0- 15   0.19    1.10     0.13      0.47     1.17   66.34      1.00    58.30      100.0%
 15- 30   0.16    0.50     0.14      0.35     1.19  312.24      0.91    81.66      100.0%
 30- 45   0.17    0.71     0.14      0.33     1.10   65.94      0.91    85.98      100.0%
 45- 60   0.17    0.48     0.20     35.41     1.10   62.07      0.97    70.16      100.0%
 60- 75   0.16    0.56     0.85     75.51     1.06   56.82      0.85    75.04       96.9%
 75- 90   0.16    0.66     2.10    803.64     1.07   60.74      0.83   605.02       82.1%
 90-105   0.23    0.82     2.24    262.36     1.07   61.02      0.80   203.50       68.1%
105-120   0.18    0.47     2.61    207.74     1.08   54.79      0.80   104.69       68.3%
120-135   0.19    0.68     2.89     86.39     1.00   61.33      0.81    56.90       65.2%
135-150   0.18    0.89     2.37    203.06     0.81   56.07      0.82   134.41       61.3%
150-165   0.30    1.37     1.95    983.97     0.92   60.31      0.81   679.37       70.6%  <- measured window starts
165-180   0.20    0.55     1.85    190.87     0.80   58.22      0.80   106.52       77.0%
180-195   0.19    0.82     2.21    190.99     0.78   53.48      0.82   113.03       75.5%
195-210   0.17    0.54     2.13    108.67     0.74   54.89      0.81    79.97       74.6%
210-225   0.18    1.08     2.36   1027.97     0.80   54.63      0.81   710.80       70.3%
225-240   0.23    0.93     1.97    157.57     0.82  132.16      0.82    73.39       70.5%
240-255   0.19    2.76     2.15    119.47     0.89   88.34      0.80    80.50       75.8%
255-270   0.18    0.84     2.05    150.71     0.78   55.12      0.80    85.20       74.9%
270-285   0.17    0.45     1.74    998.92     0.81   53.91      0.84   961.38       76.9%
285-300   0.33    2.21     1.52    223.11     0.80   52.71      0.82   150.35       80.0%
300-315   0.20    0.88     1.54     75.37     0.82   50.78      0.81    57.75       82.1%
315-330   0.20    1.32     1.30    108.02     0.82   56.11      0.81    70.48       82.0%
```

Memory fills 50 to 60 s into the one-node run. From then on a fifth to two fifths of the index is out of RAM, and over the measured window the average query pays for about 28 page-ins. The spikes near one second recur about every 65 s, which is the period of the 512 MiB merge (16 flushes at 7.8 MB/s). Merge timestamps were not logged, so that link is inferred. The index-in-RAM column is bucketed on the sampler's own clock, which starts roughly 5 to 11 s before the load clock, so it runs that far ahead of the latency columns.

## What this does and does not show

- It is a model. Real engines do far more work per query and pace their I/O more carefully. They also have more ways to hurt each other: merge and distance-computation CPU, JVM garbage collection, connection memory.
- Uniform random access is the hard case for an LRU and the normal case for HNSW traversal over a large graph. A skewed keyword workload keeps a hot set resident and would suffer less.
- The one-node case is over-committed by design: a 3 GiB index, a 3 GiB collection, a 1.5 GiB private cache and about 1 GiB of segments on a 6 GiB box. Each workload fits alone, and neither evicted a page in its measured window. The documented co-located sizing does not count the database's appetite for page cache.
- A cgroup v1 limit reclaims synchronously in the task that allocates, with no background reclaim, so a bare instance of the same size may behave differently.
- The baseline moved between sessions on the same VM, as the session table shows. Re-run on dedicated hardware before quoting absolute numbers.
- The disk is fast. On network block storage with IOPS limits, every major fault and every fsync costs more.
