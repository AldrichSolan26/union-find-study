# Union-Find: Empirical Study (PA1, Track A)

A C++17 disjoint-set (Union-Find) implementation with interchangeable
**linking** and **find** policies, plus a benchmark harness that compares all
12 combinations on three workloads.

## Build and run

Requirements: a C++17 compiler (g++ 9+ or clang 10+), CMake 3.16+, and
Python 3 with `pandas` and `matplotlib` for the plots.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

./build/uf_tests                                          # correctness tests
./build/uf_bench --workload all --out results/results.csv # about 1 minute
python3 scripts/plot.py results/results.csv               # writes results/*.png
```

Always benchmark a **Release** build. Debug timings are meaningless.

### Benchmark options

| Flag | Default | Meaning |
| --- | --- | --- |
| `--workload` | `all` | `random`, `chain`, `kruskal` or `all` |
| `--min-exp` / `--max-exp` | `10` / `20` | sweep n = 2^min … 2^max |
| `--slow-max-exp` | `15` | largest n for the naive+none variant, which is O(n²) on chain and Kruskal |
| `--reps` | `5` | timed repetitions; the median is reported |
| `--seed` | `1` | RNG seed for reproducible workloads |
| `--out` | `results/results.csv` | output CSV |

## Layout

```
include/union_find.hpp   the structure + all link and find policies
include/variants.hpp     compile-time list of the 12 variants
tests/test_union_find.cpp  oracle comparison, invariants, height bound
bench/workloads.hpp      random / chain / kruskal generators
bench/bench.cpp          timing harness -> CSV
scripts/plot.py          CSV -> one PNG per workload
docs/ai-log.md           running log of AI use (feeds the report)
results/                 your CSV and plots go here
```

## Variants

| Link policy | Find policy |
| --- | --- |
| `naive`: hang first root under second | `none`: walk to root |
| `rank`: union by rank | `full`: two-pass path compression |
| `size`: union by size | `halving`: point each node at its grandparent, jump |
|  | `splitting`: point each node at its grandparent, step |

## How the measurement works

- Workloads are generated **before** timing; Kruskal edges are pre-sorted.
- Each (workload, n, variant) gets one warm-up run, then `--reps` timed runs.
  The median is reported as `median_ns` and `ns_per_op`.
- Each run writes a `checksum` (merge count or MST weight). All variants must
  agree; the harness prints `CHECKSUM MISMATCH` and exits non-zero if not.
- Every `find()` result is folded into a `volatile` sink. Without it, the
  optimiser deleted side-effect-free finds and reported the slowest variant
  as the fastest. See the comment in `bench.cpp`.

## Before trusting your numbers

- Close other heavy programs; plug in a laptop (power saving changes clocks).
- Record CPU model, cache sizes, compiler and version in the report.
- Re-run with a different `--seed` to check the results are stable.
