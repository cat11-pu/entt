# Sparse-set traversal benchmark

The benchmark uses three scales (`10k`, `100k`, `1m` entities), shuffles
component insertion order, excludes around 10% of entities with a tag, and
destroys around 5% of them. This makes the packed arrays differ and prevents
sparse lookups from being accidentally sequential.

Run:

```sh
./bench/compare.sh
```

The script extracts the pristine `HEAD` headers into a temporary directory and
builds the same benchmark twice:

- `baseline`: headers from `HEAD`.
- `optimized`: headers from the working tree.

It runs both binaries five times, keeps the fastest sample per row, and compares
the per-scale traversal fingerprints. The fingerprint hashes the visited entity
order plus the resulting component state, so a mismatch means the optimizations
changed traversal semantics.

Reported columns:

- `view_range_ms`: `for(auto ... : view.each())`.
- `view_each_ms`: `view.each(callback)`.
- `sorted_*_ms`: the same traversals after sorting both pools once by entity
  id (`registry.sort<position>` plus `registry.sort<velocity, position>`).
  Sorting is excluded from the timed region because it is a setup-time
  operation for the usual "values change, composition does not" frame loop.
- `group_range_ms`: owning group with the same composition, used as the
  cache-locality upper bound.
- `*_ns`: best-case nanoseconds per input entity.
