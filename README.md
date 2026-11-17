# One node, two engines

A small Linux benchmark that runs a transactional database's I/O pattern and a Lucene-style search engine's I/O pattern, first each on its own node and then on one node, and records what sharing a page cache does to both.

This is the code and data behind the article *The Serious Shortcomings of Postgres and MongoDB Search Options, and Why Weaviate Is a Growing Business*, on the Substack *Marcus on AI Infra*. It is a model of two I/O patterns. It does not run mongod, mongot, Lucene or Postgres.

## The result

Each workload fits alone on a 2-vCPU node with a 6 GiB memory limit. Sharing that node, they do not:

```text
node: 2 vCPU, 6.0 GiB RAM (memory cgroup limit), Linux 6.18.44, ext4, no swap
data: 3072 MiB mmap index; 3072 MiB collection + 1536 MiB private db cache
load: 200 queries/s x 128 random pages; 250 txns/s x (4 reads + 32 KB commit)
      150 s ramp, then 180 s measured

latency        search p50  search p99     txn p50     txn p99
search alone      0.20 ms     1.08 ms           -           -
db alone                -           -     0.81 ms    59.90 ms
one node          1.92 ms   745.60 ms     0.81 ms   504.55 ms

kernel, PSI   index in RAM major faults     evicted   io some   io full  mem some
search alone        100.0%            0       0 MiB      3.4%      3.3%      0.0%
db alone                 -            -       0 MiB     17.1%     16.7%      0.0%
one node             70.3%      1018417    7551 MiB     27.1%     26.6%      0.8%
```

On the shared node the kernel pushed up to 30% of the index out of RAM, a million page touches became major faults, and some task was stalled on I/O for 27% of the measured window while memory pressure read 0.8%.

That is one session of three on one ephemeral VM. Across the three, the shared-node tail latencies stayed within about 30% of each other. The database-alone baseline did not: its transaction p99 was 5.79 ms in one session and 59.90 ms in another. Read the [reproduction notes](reproduction-notes.md) before quoting a number, and re-run on your own hardware.

## Files

| Path | Contents |
| --- | --- |
| `contend.c` | The benchmark: the `search` and `db` workload models, 355 lines of C |
| `psiwatch.c` | Counts how often PSI triggers fire |
| `run.sh` | Runs the three scenarios, samples `/proc/vmstat` and `/proc/pressure`, prints the report |
| `reproduction-notes.md` | Method, environment, results from every session, PSI detail, time series, caveats |
| `results/` | Raw logs from the three sessions |

## Run it

You need Linux with PSI enabled (`/proc/pressure` exists), gcc, root, and about 8 GiB of free disk.

```bash
gcc -O2 -pthread -o contend contend.c && gcc -O2 -o psiwatch psiwatch.c
mkdir -p data/db data/seg
./contend mkfile data/index 3072 && ./contend mkfile data/db/collection 3072
sudo bash run.sh    # about 18 minutes: search alone, db alone, one node
```

`bash run.sh report` prints the tables again from `results.log`. To print the published tables without running anything:

```bash
cp results/session-3.results.log results.log && bash run.sh report
```

Three things to know first:

- **It needs a 6 GiB node.** The sizes assume 6 GiB of RAM in total: a 3 GiB index, a 3 GiB collection and a 1.5 GiB private cache. With more memory nothing is evicted and the shared node looks healthy. The published runs used a cgroup v1 memory cgroup limited to 6 GiB, on a VM with no swap. A 6 GiB VM or a cgroup v2 limit should show the same effect, but neither has been tested, and reclaim differs between them (see the notes).
- **It is hard on the machine.** `run.sh` drops the system page cache before each scenario, then drives the node into memory and I/O pressure on purpose. Do not run it next to work you care about.
- **PSI readings are system-wide.** `run.sh` reads `/proc/pressure`, so anything else running on the machine shows up in them.

Load and timing are environment variables, with their defaults at the top of `run.sh`: `QPS`, `PAGES`, `TXNS`, `COMMIT_KB`, `READS`, `CACHE_MB`, `RAMP`, `SECS`, `WARM`. File sizes are the `mkfile` arguments. Set `CONTEND_TS=<file>` (`sudo CONTEND_TS=timeseries.log bash run.sh`) to also append 15 s latency buckets and 5 s samples of how much of the index is in RAM.

## What it models

- `contend search` stands in for a Lucene-style engine: a 3 GiB mmap'd index read at random, 200 queries/s of 128 pages each, plus an indexer that writes the incoming documents again as segments and merges them.
- `contend db` stands in for a transactional engine: a 1.5 GiB private cache of anonymous memory and 250 transactions/s, each with point reads, a WAL append with `fdatasync` and in-place page writes, plus a checkpointer.

Both loops are open: latency runs from each request's scheduled arrival, so time spent queued counts. Sizing follows the co-located sizing in [MongoDB's deployment docs](https://www.mongodb.com/docs/vector-search/deployment/deployment-options/): half the RAM for the index, a quarter for the database.

## Limits

This is a model, on one small VM, with the shared node over-committed by design. Real engines do more work per query, pace their I/O more carefully, and have more ways to hurt each other. A skewed keyword workload would suffer less than the uniform random access modelled here. The full list closes the [reproduction notes](reproduction-notes.md#what-this-does-and-does-not-show).

## Background

- [PSI: Pressure Stall Information](https://docs.kernel.org/accounting/psi.html), Linux kernel documentation
- [Memory management concepts](https://docs.kernel.org/admin-guide/mm/concepts.html), Linux kernel documentation

## License

MIT. See [LICENSE](LICENSE).
