#!/usr/bin/env bash
set -euo pipefail

bench=${1:-build-profile/strata_bench}
label=${2:-current}
output=${3:-results}
ops=${4:-300000}
perf_command=("${STRATA_PERF:-perf}")
profiled_user=()
if [[ ${STRATA_PERF_SUDO:-0} == 1 ]]; then
    perf_command=(sudo -n "${perf_command[@]}")
    profiled_user=(sudo -n -u "$(id -un)" --)
fi
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
mkdir -p "$output"

"${perf_command[@]}" record -e cpu-clock:u -F 499 -o "$scratch/perf.data" -- \
    "${profiled_user[@]}" "$bench" readrandom 1 always "$scratch/profile" "$ops"
"${perf_command[@]}" report --stdio --no-children --percent-limit 0.5 \
    -i "$scratch/perf.data" | sed 's/[[:blank:]]*$//' > "$output/perf-$label.txt"

echo 'workload,threads,sync_mode,operations,ops_per_sec,p50_us,p99_us,p999_us' \
    > "$output/read-profile-$label.csv"
for run in 1 2 3; do
    "$bench" readrandom 1 always "$scratch/run-$run" "$ops" |
        tee -a "$output/read-profile-$label.csv"
done
