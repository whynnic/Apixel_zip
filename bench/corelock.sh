#!/bin/bash
# Run one single-threaded timing job on its own physical core, so up to 5 timings run at once.
#
# usage: bench/corelock.sh <command> [args...]
#        APX_TIME_FILE=<f> bench/corelock.sh ...   also write wall seconds of the command to <f>; the clock
#                                                 starts after the core is acquired, so waiting is not timed
#
# Ryzen 5 8600G: physical cores 0..5, SMT siblings 6..11 (lscpu -e). Core 0 is left to the system, builds
# and agents; cores 1..5 are timing slots /tmp/apixel-core-<k>.lock. The job is pinned to core k.
#
# Whole-machine runs (12-thread timings, the final competitor table) take /tmp/apixel-bench.lock
# EXCLUSIVELY (flock -x); this script holds it SHARED, so it waits while such a run is active and
# blocks it from starting while slot jobs run.
#
# Under background load absolute MiB/s drift; decisions use interleaved A/B on the same slot (min of N),
# whose ratio stays within a few percent. Absolute numbers for BENCHMARKS.md come from exclusive runs.
set -euo pipefail
[ $# -ge 1 ] || { echo "usage: $0 <command> [args...]" >&2; exit 2; }
exec 199>/tmp/apixel-bench.lock
flock -s 199
while :; do
    for k in 1 2 3 4 5; do
        exec 198>"/tmp/apixel-core-$k.lock"
        if flock -n 198; then
            if [ -z "${APX_TIME_FILE:-}" ]; then APX_CORE=$k exec taskset -c "$k" "$@"; fi
            t0=$(date +%s%N); rc=0
            APX_CORE=$k taskset -c "$k" "$@" || rc=$?
            t1=$(date +%s%N)
            echo "$(( (t1 - t0) / 1000000 ))" | awk '{printf "%.3f\n", $1 / 1000}' > "$APX_TIME_FILE"
            exit "$rc"
        fi
        exec 198>&-
    done
    sleep 0.5
done
