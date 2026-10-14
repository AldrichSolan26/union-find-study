# Union-Find: Empirical Study (PA1, Track A)

A C++17 disjoint-set (Union-Find) implementation with interchangeable
**linking** and **find** policies, zero-cost instrumentation, and a
benchmark harness that compares all 12 combinations on six workloads, from
n = 2^10 to 2^24.

The study asks where the textbook analysis (near-constant amortised cost,
O(m α(n))) agrees with measured run time, where it doesn't, and why.

- **Report** (hypotheses, method, results, analysis, AI use): [report/PA1_Report_Union_Find.docx](report/PA1_Report_Union_Find.docx)
- **Generated tables:** [results/summary.md](results/summary.md) · **figures:** [results/figures/](results/figures/)
- **Pre-registration:** hypotheses H1–H7 were committed before the full run, in
  commit `5f9148f` (`docs/hypotheses.md` in that commit).

**In one paragraph:**
- The textbook analysis predicts the *work* exactly: path lengths match the
  closed-form worst cases, and stay flat with balanced linking plus
  compression.
- It does not predict the *time*: with work flat, time per operation grows
  3.0–3.7× from n = 2^10 to 2^24, in steps at the cache sizes.
- Path halving is the fastest find policy in 149 of 180 configurations.
- Three measurement pitfalls (dead-code elimination, a branch predictor
  memorising replayed inputs, Windows power throttling) each distorted the
  results by 2× or more until they were found and fixed.

## Build

Requirements: a C++17 compiler (g++ 9+, clang 10+ or MSVC 2019+) and either
`make` or CMake 3.16+. The study scripts need Python 3.9+. Plotting also needs
`pandas` and `matplotlib`.

**With make** (Linux, macOS, or MSYS2/MinGW on Windows):

```bash
make            # builds build/uf_tests, build/uf_bench, build/uf_bench_nosink
make test       # builds and runs the tests
```

On Windows with MSYS2, the make binary is called `mingw32-make`.

**With CMake:**

```bash
cmake -B build                       # single-config generators default to Release
cmake --build build --config Release # --config matters for Visual Studio / Xcode
ctest --test-dir build -C Release    # or run build/uf_tests directly
```

Always benchmark an optimised build. `uf_bench --build-info` reports the
compiler, whether `NDEBUG` and optimisation are on, the clock resolution, and
the caches of the core it is pinned to. A debug build also prints a warning
every time it runs.

## Run

```bash
./build/uf_tests                              # ~0.3 s; prints "N checks, 0 failures"
python scripts/run_study.py --quick           # smoke run, ~1.5 min (n <= 2^16, 1 seed)
python scripts/run_study.py                   # the full study, ~1-1.5 h (see below)
python scripts/plot.py results               # figures + results/summary.md
```

`run_study.py` writes `results/machine.json` (CPU, caches, OS, compiler,
settings), one CSV per (workload, seed) in `results/raw/` (working files,
not committed), the merged `results/results.csv`, `results/canary.csv` and
`results/validity.csv`. Use
`--resume` to continue an interrupted run (`--no-validity` skips the
validity experiments). `--cpu N` chooses the core to pin to (default 4). On a
hybrid Intel CPU, pick a performance core: `uf_bench --cpu N --build-info`
says which kind of core N is. On Windows, the runner stops the machine
idle-sleeping for the duration, but closing the lid still sleeps it.

The side experiments behind three of the findings build with
`make experiments`; see [experiments/README.md](experiments/README.md).

`uf_bench` can also be run directly:

| Flag | Default | Meaning |
| --- | --- | --- |
| `--workload` | `all` | comma-separated: `random`, `chain`, `binomial`, `local`, `local-shuffled`, `kruskal` |
| `--min-exp` / `--max-exp` | `10` / `20` | sweep n = 2^min … 2^max (max 26) |
| `--slow-max-exp` | `15` | largest n for naive+none, which is O(n) per find on chain-like inputs |
| `--seeds` | `1` | comma-separated RNG seeds; one set of rows per seed |
| `--min-reps` / `--max-reps` / `--min-time-ms` | `5` / `51` / `50` | timed reps: at least min-reps and min-time-ms of work, at most max-reps |
| `--cpu` | none | pin to this logical CPU (Windows and Linux) |
| `--variants` | all | comma-separated subset, e.g. `rank+full,size+halving` |
| `--no-count` | off | skip the instrumented counting run |
| `--out` | `results/results.csv` | output CSV |
| `--replay-same-sequence` | off | validity experiment: time one sequence over and over |
| `--allow-power-throttling` | off | validity experiment (Windows): don't opt out of power throttling |
| `--build-info` | | print build and machine details and exit |

## Layout

```
include/union_find.hpp     the structure + all link, find and stats policies
include/variants.hpp       compile-time list of the 12 variants
bench/workloads.hpp        the six workload generators (platform-independent RNG)
bench/harness.hpp          the timed code (run_ops, run_kruskal) and forest_shape
bench/bench.cpp            timing harness -> CSV
tests/test_union_find.cpp  correctness tests (see below)
scripts/run_study.py       runs the whole study, records the machine
scripts/plot.py            CSV -> figures + summary.md
experiments/               standalone side experiments (H5, H8, H9) and their outputs
report/                    the written report (Word)
results/                   the final data: results.csv, validity.csv, canary.csv,
                           machine.json, summary.md and figures/
```

Data that was measured and then re-measured (after a laptop sleep, a noisy
first session, and one transient slowdown), the per-chunk CSVs and the run
logs are not in the current tree. They are in the git history, at commit
`0d9841c` (`results/discarded/` explains each case).

Built and tested with g++ 16.2 (MSYS2), clang 23 (both MSVC and MinGW
targets), make, and CMake 4.4 with the MinGW Makefiles generator. The clang
build with Microsoft's standard library reproduces every checksum and work
count of the g++ build.

## Variants

Each variant is `UnionFind<Link, Find, Stats>`, with the policies as template
parameters: no virtual calls, and the uninstrumented `NoStats` compiles away.

| Link policy | Find policy |
| --- | --- |
| `naive`: hang the first root under the second | `none`: walk to the root |
| `rank`: union by rank (1 byte per element) | `full`: two-pass path compression |
| `size`: union by size (4 bytes per element) | `halving`: re-point every other node to its grandparent |
|  | `splitting`: re-point every node to its grandparent |

## Workloads

| Name | Operations (n elements) | Why it is there |
| --- | --- | --- |
| `random` | 2n: half unite, half connected, uniform endpoints | the typical case |
| `chain` | unite(i, i+1) for all i, then n uniform finds | worst case for naive linking (a path of length n−1) |
| `binomial` | unite(i, i+s) for s = 1, 2, 4, …, then n uniform finds | worst case for rank/size linking (height exactly log2 n) |
| `local` | like random, but b is within 16 of a | cache-friendly addresses |
| `local-shuffled` | `local` with ids relabelled by a random permutation | **identical work** to `local` (tested), scattered addresses |
| `kruskal` | Kruskal's MST on 4n random edges plus a heavy spanning path | a real algorithm on top of Union-Find |

## How the measurement works

- **Only Union-Find work is timed.** Workloads are generated and Kruskal
  edges sorted before timing. Each timed run gets a freshly constructed
  structure, and construction is not timed.
- **Time and work are measured separately.** An untimed counting run with
  `CountStats` records parent-array reads and writes, the path length per
  find, and the final forest's depth. Those numbers are machine-independent.
  The timed runs use the uninstrumented type.
- **Repetitions:** one warm-up, then at least `--min-reps` runs and
  `--min-time-ms` of timed work. The CSV reports the median, quartiles and
  minimum. The full study uses 3 seeds, and the plots show the median over
  seeds with min–max bands.
- **Checksums:** every run returns a checksum (merge count plus "yes"
  answers, or the MST weight). All variants must agree, and every timed run
  must match a reference answer for its sequence. Otherwise the harness
  prints `CHECKSUM MISMATCH` and exits non-zero.

### Safeguards, and the experiments that justify them

Each safeguard was added because, without it, the numbers were wrong. Each
one can be switched off to reproduce the failure; `run_study.py` does this
and saves the results in `results/validity.csv`.

| Safeguard | Without it | Reproduce with |
| --- | --- | --- |
| Every `find()` result is folded into a `volatile` sink | the optimiser deletes side-effect-free finds; naive+none on `chain` looks like the fastest variant | `uf_bench_nosink` |
| Short sequences get a fresh sequence per timed rep | the branch predictor memorises a replayed sequence; small n looks up to ~4× faster | `--replay-same-sequence` |
| Opt out of Windows power throttling | after ~5 s of load Windows clocks the core down ~2.3×, so results depend on the order the variants run in | `--allow-power-throttling` |
| Pin to one performance core | on hybrid CPUs the thread can migrate between P- and E-cores | omit `--cpu` |
| Drift monitor (`canary.csv`) | a slow change in machine speed would go unnoticed | — |

### Before trusting your own numbers

- Plug in a laptop and close other heavy programs.
- `results/machine.json` records the CPU, the pinned core's caches, the OS and
  power plan, and the compiler. Cite these with any numbers.
- Check `canary.csv`: its max/min ratio should be close to 1.
- Re-run with different `--seeds`. `summary.md` reports the spread across seeds.

## Tests

`tests/test_union_find.cpp` (no framework, ~7,300 checks) covers:

- **Edge cases:** n = 0 and n = 1, self-union, repeated union, out-of-range
  arguments and negative n (these throw).
- **Agreement with a brute-force oracle** on random operations.
- **Structural invariants** after every batch of operations: no cycles,
  `rank[parent[x]] > rank[x]`, `|set(r)| ≥ 2^rank[r]`, and `size[root]`
  equal to the true set size.
- **The log2 n height bound**, and the binomial workload reaching it exactly.
- **What each find policy does to a path:** the exact parent pointers after
  one find on a chain.
- **Instrumentation:** exact read/write counts on a known path, and identical
  behaviour with and without counting.
- **Workloads:** `local` and `local-shuffled` do identical work; Kruskal's MST
  weight matches an independent Prim's algorithm; every variant gets the same
  checksum on every workload.

The tests were themselves checked by deliberately breaking the library (for
example, dropping the `ra == rb` check, or making halving step to the parent
instead of the grandparent). The test suite caught all eight injected bugs.
