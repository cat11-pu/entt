#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

CXX="${CXX:-g++}"
FLAGS=(-std=c++20 "${BENCH_OPT:--O3}" -DNDEBUG -march=native)
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

run=()
if command -v taskset >/dev/null 2>&1; then
    run=(taskset -c "${BENCH_CPU:-2}")
fi

mkdir -p "$tmp/baseline" bench/.bin
git archive HEAD | tar -x -C "$tmp/baseline"

"$CXX" "${FLAGS[@]}" -I "$tmp/baseline/src" bench/view_iteration.cpp -o bench/.bin/baseline
"$CXX" "${FLAGS[@]}" -I src bench/view_iteration.cpp -o bench/.bin/optimized

for round in 1 2 3 4 5; do
    "${run[@]}" bench/.bin/baseline >"$tmp/baseline.$round.csv"
    "${run[@]}" bench/.bin/optimized >"$tmp/optimized.$round.csv"
done

show_best() {
    local label="$1"
    local prefix="$2"
    echo "== $label (best of 5 runs) =="
    tail -q -n +2 "$tmp/$prefix".*.csv | awk -F, '
        !seen[$1]++ {
            order[++rows] = $1
            for (field = 2; field <= NF; ++field) best[$1, field] = $field
        }
        {
            for (field = 2; field <= NF; ++field) {
                if ($field < best[$1, field]) best[$1, field] = $field
            }
        }
        END {
            print "entities,view_range_ms,view_each_ms,sorted_range_ms,sorted_each_ms,group_range_ms,view_range_ns,view_each_ns,sorted_range_ns,sorted_each_ns,group_range_ns"
            for (row = 1; row <= rows; ++row) {
                key = order[row]
                printf "%s", key
                for (field = 2; field <= 11; ++field) printf ",%.3f", best[key, field]
                print ""
            }
        }
    '
}

show_best "baseline" baseline
show_best "optimized" optimized

bench/.bin/baseline d >"$tmp/baseline.fingerprint"
bench/.bin/optimized d >"$tmp/optimized.fingerprint"

if diff -u "$tmp/baseline.fingerprint" "$tmp/optimized.fingerprint"; then
    echo "fingerprints: identical"
else
    echo "fingerprints: mismatch" >&2
    exit 1
fi
