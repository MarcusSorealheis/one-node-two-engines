#!/usr/bin/env bash
# run.sh: the same two workloads, first each on its own node, then on one node.
# "Own node" = the process gets this whole box to itself. Needs root (drop_caches, PSI triggers).
#   ./run.sh [search] [db] [both]   run scenarios (default: all three), then print the report
#   ./run.sh report                 print the report again from results.log
# Setup once:
#   gcc -O2 -pthread -o contend contend.c && gcc -O2 -o psiwatch psiwatch.c
#   mkdir -p data/db data/seg
#   ./contend mkfile data/index 3072 && ./contend mkfile data/db/collection 3072
set -euo pipefail
cd "$(dirname "$0")"

RAMP=${RAMP:-150}            # seconds of load before measuring (steady state)
SECS=${SECS:-180}            # measured window per scenario
QPS=${QPS:-200}              # search queries per second (open loop)
PAGES=${PAGES:-128}          # random index pages touched per query
CACHE_MB=${CACHE_MB:-1536}   # db engine's private cache (anonymous memory)
TXNS=${TXNS:-250}            # db transactions per second (open loop)
COMMIT_KB=${COMMIT_KB:-32}   # bytes written per txn; the indexer sees the same stream
READS=${READS:-4}            # 8 KB point reads per txn that miss the db cache
WARM=${WARM:-12}             # seconds allowed for cache fill + index preload
RATE_MB=$(awk -v c="$TXNS" -v k="$COMMIT_KB" 'BEGIN{printf "%.1f", c*k/1024}')

field() { sed -n "s/.*[ ]$1=\([^ ]*\).*/\1/p" <<<" $2"; }
ms()    { [ -z "$1" ] && printf '%s' '-' || awk -v u="$1" 'BEGIN{printf "%.2f ms", u/1000}'; }
mib()   { echo "$(( ${1:-0} * 4 / 1024 )) MiB"; }
pct()   { local v; v=$(field "$1" "$2"); if [ -n "$v" ]; then echo "$v%"; else echo "-"; fi; }

report() {
  grep '^## ' results.log | cut -c4-
  echo
  printf '%-13s %11s %11s %11s %11s\n' latency "search p50" "search p99" "txn p50" "txn p99"
  grep -v '^## ' results.log | while IFS='|' read -r label so do_ kern; do
    printf '%-13s %11s %11s %11s %11s\n' "$label" \
      "$(ms "$(field p50_us "$so")")" "$(ms "$(field p99_us "$so")")" \
      "$(ms "$(field p50_us "$do_")")" "$(ms "$(field p99_us "$do_")")"
  done
  echo
  # PSI columns: share of the measured window with some / all non-idle tasks stalled
  printf '%-13s %12s %12s %11s %9s %9s %9s\n' "kernel, PSI" "index in RAM" "major faults" evicted "io some" "io full" "mem some"
  grep -v '^## ' results.log | while IFS='|' read -r label so do_ kern; do
    res=$(field resident_min_pct "$so"); flt=$(field majflt "$so")
    if [ -n "$res" ]; then res="$res%"; else res="-"; fi
    printf '%-13s %12s %12s %11s %9s %9s %9s\n' "$label" "$res" "${flt:--}" \
      "$(mib "$(field pgsteal_file "$kern")")" "$(pct psi_io_some_pct "$kern")" \
      "$(pct psi_io_full_pct "$kern")" "$(pct psi_mem_some_pct "$kern")"
  done
}
if [ "${1:-}" = report ]; then report; exit 0; fi

CG=/sys/fs/cgroup/memory$(awk -F: '$2=="memory"{print $3}' /proc/self/cgroup)   # cgroup v1, if any
LIMIT=$(cat "$CG/memory.limit_in_bytes" 2>/dev/null || echo 0)
TOTAL=$(awk '/MemTotal/{print $2*1024}' /proc/meminfo)
if [ "$LIMIT" -gt 0 ] && [ "$LIMIT" -lt "$TOTAL" ]; then RAM="$(awk -v b="$LIMIT" 'BEGIN{printf "%.1f", b/2^30}') GiB RAM (memory cgroup limit)"
else RAM="$(awk -v b="$TOTAL" 'BEGIN{printf "%.1f", b/2^30}') GiB RAM"; fi

vm()    { awk -v k="$1" '$1==k{print $2}' /proc/vmstat; }
psi()   { awk -v k="$2" '$1==k{split($5,a,"="); print a[2]}' "/proc/pressure/$1"; }   # total stall time, us
snap()  { echo "$(vm workingset_refault_file) $(vm pgsteal_file) $(psi io some) $(psi io full) $(psi memory some) $(psi memory full) $(psi cpu some)"; }
share() { awk -v a="$1" -v b="$2" -v s="$SECS" 'BEGIN{printf "%.1f", (b-a)/(s*1e4)}'; }  # % of the measured window
reset() { rm -f data/seg/seg_*; sync; echo 3 > /proc/sys/vm/drop_caches; sleep 1; }

{
  echo "## node: $(nproc) vCPU, $RAM, Linux $(uname -r | cut -d- -f1), ext4, no swap"
  echo "## data: $(( $(stat -c %s data/index) >> 20 )) MiB mmap index; $(( $(stat -c %s data/db/collection) >> 20 )) MiB collection + ${CACHE_MB} MiB private db cache"
  echo "## load: ${QPS} queries/s x ${PAGES} random pages; ${TXNS} txns/s x (${READS} reads + ${COMMIT_KB} KB commit)"
  echo "##       ${RAMP} s ramp, then ${SECS} s measured"
} > results.log
: > psi.raw.log

for s in ${*:-search db both}; do
  reset
  t0=$(awk -v w="$WARM" 'BEGIN{srand(); printf "%.3f", systime()+w}')
  so=""; do_=""
  case $s in
    search) ./contend search data/index "$RAMP" "$SECS" "$QPS" "$PAGES" data/seg "$RATE_MB" "$t0" > .search.out & ;;
    db)     ./contend db data/db "$RAMP" "$SECS" "$CACHE_MB" "$TXNS" "$COMMIT_KB" "$READS" "$t0" > .db.out & ;;
    both)   ./contend db data/db "$RAMP" "$SECS" "$CACHE_MB" "$TXNS" "$COMMIT_KB" "$READS" "$t0" > .db.out &
            ./contend search data/index "$RAMP" "$SECS" "$QPS" "$PAGES" data/seg "$RATE_MB" "$t0" > .search.out & ;;
    *) echo "unknown scenario $s" >&2; exit 2 ;;
  esac
  label="one node"; [ "$s" = search ] && label="search alone"; [ "$s" = db ] && label="db alone"
  sleep "$(awk -v t="$t0" -v r="$RAMP" 'BEGIN{srand(); d=t+r-systime(); print (d>0)?d:0}')"
  read -r r0 e0 is0 if0 ms0 mf0 cs0 <<<"$(snap)"   # kernel counters at the start of the measured window
  # The kernel PSI documentation's two example triggers (memory "some 150000 1000000", io
  # "full 50000 1000000"), at the same rates over a 2 s window: without CAP_SYS_RESOURCE the
  # window must be a multiple of 2 s. Each count is the number of 2 s windows that crossed it.
  : > .psi.out
  ./psiwatch "$SECS" /proc/pressure/memory "some 300000 2000000" /proc/pressure/io "full 100000 2000000" > .psi.out 2>/dev/null &
  wait
  read -r r1 e1 is1 if1 ms1 mf1 cs1 <<<"$(snap)"
  { echo "# $label, end of measured window"; grep . /proc/pressure/io /proc/pressure/memory; } >> psi.raw.log
  tm=""; ti=""; read -r tm ti < .psi.out || true
  [ "$s" != db ] && so=$(cat .search.out); [ "$s" != search ] && do_=$(cat .db.out)
  kern="refault_file=$(( r1 - r0 )) pgsteal_file=$(( e1 - e0 ))"
  kern="$kern psi_io_some_pct=$(share "$is0" "$is1") psi_io_full_pct=$(share "$if0" "$if1")"
  kern="$kern psi_mem_some_pct=$(share "$ms0" "$ms1") psi_mem_full_pct=$(share "$mf0" "$mf1") psi_cpu_some_pct=$(share "$cs0" "$cs1")"
  kern="$kern trig_mem_some=${tm:--} trig_io_full=${ti:--}"
  echo "$label|$so|$do_|$kern" >> results.log
done
report
