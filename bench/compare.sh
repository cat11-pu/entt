#!/usr/bin/env bash
# Interleaves benchmark binaries and reports the best (min) time per row,
# which is robust against noisy neighbors on a shared machine.
set -u
cd "$(dirname "$0")"

bins=("$@")
[ ${#bins[@]} -eq 0 ] && bins=(bench_base bench_opt)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

for round in 1 2 3; do
    for bin in "${bins[@]}"; do
        "./$bin" >"$tmp/$(basename "$bin").${round}.txt"
    done
done

for bin in "${bins[@]}"; do
    echo "== $bin (best of runs) =="
    cat "$tmp/$(basename "$bin")".*.txt | awk '
        /entities/ {
            key = $1
            rf = $5; ef = $9
            if (!(key in minrf) || rf < minrf[key]) minrf[key] = rf
            if (!(key in minef) || ef < minef[key]) minef[key] = ef
        }
        END {
            for (key in minrf) printf "%9d entities | range-for %8.3f ms | each(func) %8.3f ms\n", key, minrf[key], minef[key]
        }' | sort -n
done
