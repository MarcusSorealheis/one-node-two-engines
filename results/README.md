# Raw results

Three sessions on the same VM on 13 November 2026, with the same `contend.c` and the same parameters. Session 3 is the one the article quotes.

| Session | Scenarios | Files |
| --- | --- | --- |
| 1 | search alone, db alone, one node | `session-1.results.log`, `session-1.report.txt`, `session-1.timeseries.log` |
| 2 | one node only | `session-2.results.log`, `session-2.report.txt`, `session-2.timeseries.log` |
| 3 | search alone, db alone, one node | `session-3.results.log`, `session-3.psi.raw.log`, `session-3.report.txt`, `session-3.timeseries.log` |

## Formats

- `*.results.log`: one line per scenario, holding the output of each `contend` process and the kernel counters for the measured window. Session 3 is exactly what the `run.sh` in this repository writes, `label|search|db|kernel`. Copy it to `results.log` in the repository root and `bash run.sh report` prints the tables.
- Sessions 1 and 2 were recorded by an earlier revision of `run.sh`, before it captured PSI in full. Their lines read `# label: search | db | kernel` (a scenario leaves out the process it did not run), carry a system-wide major-fault count (`sys_majflt`) that the current script dropped, and have no PSI fields. For those sessions the I/O stall share appears only in the report, in the `io stall` column (PSI `io some`). The reproduction notes show the same numbers laid out in the current format.
- `*.report.txt`: the report as printed at the end of the run.
- `session-3.psi.raw.log`: `/proc/pressure/io` and `/proc/pressure/memory` at the end of each measured window.
- `*.timeseries.log`: `CONTEND_TS` output. `search` and `db` lines are 15 s buckets of latency in microseconds, with `t` counted from the start of the ramp. `resident` lines sample the share of the index in RAM every 5 s, with `t` counted from when sampling starts, a few seconds before the load does. The scenarios follow each other in the order they ran, and each `contend` process writes its buckets when it exits, so on the shared node the `db` buckets come before the `search` buckets.
