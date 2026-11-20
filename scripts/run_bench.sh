#!/usr/bin/env bash
set -euo pipefail

bench=${1:-build-release/strata_bench}
ops=${2:-1000000}
output=${3:-results/bench.csv}
mkdir -p "$(dirname "$output")"
scratch=$(mktemp -d)
temporary=$(mktemp "${output}.XXXXXX")
trap 'rm -rf "$scratch"; rm -f "$temporary"' EXIT
echo 'workload,threads,sync_mode,operations,ops_per_sec,p50_us,p99_us,p999_us' > "$temporary"
for workload in fillseq fillrandom readrandom mixed; do
    for threads in 1 2 4 8; do
        for sync in always batch; do
            "$bench" "$workload" "$threads" "$sync" "$scratch/$workload-$threads-$sync" "$ops" |
                tee -a "$temporary"
        done
    done
done
mv "$temporary" "$output"
