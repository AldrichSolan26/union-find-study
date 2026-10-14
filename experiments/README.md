# Side experiments

Small standalone programs, each written to explain one result of the main
study. Unlike `uf_bench`, each one isolates a single question. Their outputs on
the study machine are in [results/](results/).

```bash
make experiments                    # or: cmake --build build --config Release
./build/replay_vs_fresh [cpu]
./build/throttling [cpu]            # then again with --opt-out
./build/rank_branch [cpu]
```

They pin to one CPU (default 4) and, except `throttling` without `--opt-out`,
opt out of Windows power throttling, exactly as `uf_bench` does.

| Program | Question | Result on the study machine |
| --- | --- | --- |
| `replay_vs_fresh` | H8: does replaying one op sequence let the branch predictor learn it? | rank+full at n = 2^10: **3.7 ns/op replayed vs 17.7 fresh**; the gap closes by n ≈ 2^15 |
| `throttling` | H9: does sustained load slow later measurements, and does a pause undo it? | throttling on: 25 → **43 ns/op** after 5 s of load, still 43 after a 3 s pause; opt-out: 18.6–19.7 throughout |
| `rank_branch` | H5: is rank's `if (rank[a] == rank[b]) ++rank[a]` why size is faster at small n? | branch-free rank closes about half the gap at n ≤ 2^12 and nearly all of it from 2^14 (2^16: rank 16.0, size 15.2, branch-free rank 15.3) |

## Caveats

- **The first version of `common.hpp` had the H7 bug.** It discarded the
  checksum `run_ops` returns, so the compiler could delete the side-effect-free
  `connected()` queries. rank+none then appeared 35% faster (14.4 vs 22.7
  ns/op at n = 2^10) than `uf_bench` measures. Every run's checksum is now
  folded into a `volatile` sink, and the results in `results/` come from the
  fixed version.
- **Absolute numbers here are about 10% lower than `uf_bench`'s** for the same
  variant, n and workload (e.g. rank+full random at n = 2^10: 17.7 here, 19.4
  in the study). Both programs run the same library code on equivalent
  sequences. The difference comes from how the compiler inlines the timed
  loop into its caller. Compare numbers within one program, not across them.
