# Results

Environment:

- AMD Ryzen 7 9700X, one pinned core (`taskset -c 2`).
- GCC 15.2, `-std=c++20 -O3 -DNDEBUG -march=native`.
- Best of 5 process runs, each with 11 measured traversals.
- Baseline is the pristine `HEAD` header set; optimized is the working tree.

## Dense leading traversal

`view::each()` range-for, shuffled storage order, 10% excluded, 5% destroyed:

| Entities | Baseline | Per entity | Optimized | Per entity | Reduction |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 10,000 | 0.032 ms | 3.22 ns | 0.016 ms | 1.61 ns | 50.0% |
| 100,000 | 0.542 ms | 5.42 ns | 0.338 ms | 3.39 ns | 37.6% |
| 1,000,000 | 15.551 ms | 15.55 ns | 7.928 ms | 7.93 ns | 49.0% |

## Access-order sorting

Both pools sorted once by entity id before the timed frame loop. Sorting time is
excluded because composition is assumed stable while values change per frame.

| Entities | Baseline | Sorted range | Per entity | Reduction | Sorted callback | Per entity | Reduction |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10,000 | 0.032 ms | 0.023 ms | 2.30 ns | 28.1% | 0.019 ms | 1.93 ns | -18.8% |
| 100,000 | 0.542 ms | 0.236 ms | 2.36 ns | 56.5% | 0.198 ms | 1.98 ns | 35.7% |
| 1,000,000 | 15.551 ms | 2.432 ms | 2.43 ns | 84.4% | 2.051 ms | 2.05 ns | 68.6% |

At 10k the working set already fits in cache, so sorting does not pay for
itself. At 100k and above it makes the sparse page and dense payload accesses
monotonic and produces the largest gain.

## Owning group upper bound

| Entities | Group range-for | Per entity |
| ---: | ---: | ---: |
| 10,000 | 0.005 ms | 0.46 ns |
| 100,000 | 0.046 ms | 0.46 ns |
| 1,000,000 | 0.510 ms | 0.51 ns |

The owning group is about 12x faster than baseline and 7x faster than the
optimized unsorted view at 100k because owned pools share one dense order and
need no sparse lookup during traversal.

## Determinism

The benchmark hashes the visited entity sequence and resulting component state
for every path and scale. Baseline and optimized fingerprints are identical:

| Entities | View range / callback / group | Sorted view |
| ---: | ---: | ---: |
| 10,000 | `167284c2f0d6c53f` | `8fa20816e09ebe8d` |
| 100,000 | `72c577aecbcd8657` | `7ea0e98fcfcc0c7f` |
| 1,000,000 | `4e4ccfaf97e75bdc` | `0373753da50e74e0` |

Reproduce with `./bench/compare.sh`.
