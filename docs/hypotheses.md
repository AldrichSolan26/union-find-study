# Hypotheses

Written and committed **before** the full benchmark run, so the results can be
checked against them rather than rationalised afterwards. They were informed by
a smoke run (n ≤ 2^16, one seed) and a pilot at n = 2^22 (random and kruskal
only), both made while building the harness. Verdicts are in
[results/FINDINGS.md](../results/FINDINGS.md).

Machine context: Intel Core i9-14900HX, run pinned to one performance core
with 48 KB L1d, 2 MB L2 and a shared 36 MB L3 (see `results/machine.json`). The
parent array is 4n bytes. Union-by-rank adds n bytes, union-by-size adds 4n.
So the main arrays outgrow L2 around n ≈ 2^18–2^19 and L3 around n ≈ 2^22–2^23.

## Theory being tested

| Variant | Worst-case bound for m operations on n elements |
| --- | --- |
| naive + none | Θ(n) per find: a chain of length n − 1 is possible |
| rank/size + none | O(log n) per find: height ≤ log2 n |
| naive + compression | O(m log_{1+m/n} n) total (Tarjan & van Leeuwen 1984) |
| rank/size + full / halving / splitting | O(m α(n)) total, and α(n) ≤ 4 for any practical n |

## Predictions

**H1. Work follows the theory.** Measured in parent-array reads per operation
(machine-independent):

- naive+none on `chain` grows linearly in n;
- rank/size+none on `binomial` grows like log2 n, with mean path ≈ (log2 n)/2;
- every rank/size + compression variant is flat in n on every workload;
- naive + compression grows, but slowly (at most logarithmically).

**H2. Time does not follow the theory.** For rank/size + compression, reads/op
stay flat but ns/op rises with n, by more than 2× between n = 2^10 and
n = 2^24. Most of the rise happens where the arrays outgrow L2 and L3. In
other words, past the caches the cost per operation is set by cache misses,
not by α(n).

**H3. Locality alone explains a large part of H2.** `local` and
`local-shuffled` do identical work (the tests check this). Their times are
within ~10% while the arrays fit in L2, and `local-shuffled` is at least 2×
slower at n = 2^24.

**H4. The choice of compression matters much less than having some.** Among
full, halving and splitting with rank/size linking, the spread in ns/op is
under ~25% at any n, against 2× or more for "compression vs none" on random
and kruskal at large n. The one-pass methods (halving, splitting) are at least
as fast as two-pass full compression at large n, because full walks the path
twice.

**H5. Rank beats size at large n, only because it is smaller.** The two build
different but similarly shallow forests (similar reads/op), but the rank array
is 1 byte per element against 4 for size. Prediction: rank+X is faster than
size+X at n ≥ 2^22, and not meaningfully different at n ≤ 2^16.

**H6. Compression without balanced linking is "good enough" on random inputs
but not free.** naive + compression does more reads per op than rank +
compression, and is slower, by less than 3× on `random` at every n.

**H7. Measurement validity.** Building the harness without the `volatile`
sink (`uf_bench_nosink`) lets the compiler delete side-effect-free finds, and
naive+none on `chain` then looks like the fastest variant. (One quick check
during development already showed this at n ≤ 2^14. The study repeats it
properly.) If this holds, the sink is necessary and the main results are only
valid with it.

## Added after the first full run started (not pre-registered)

> **Note added later.** The side-test numbers quoted in H8 and H9 below came
> from scratch programs whose timing helper discarded the `run_ops`
> checksum: the dead-code pitfall of H7, see `docs/ai-log.md`. That mostly
> affects variants without compression, which these tests did not use. Both
> effects were then measured properly with `uf_bench` (`results/validity.csv`):
> H8 is 5.4 vs 19.2 ns/op, and H9 is 2.30–2.35×. The fixed side experiments
> are in `experiments/`.

**H8. Replaying one short sequence lets the branch predictor learn it.** The
first full run timed every rep on the same op sequence. In its small-n results,
ns/op roughly tripled between n = 2^10 and 2^12, although both sizes fit in
L1. A side test showed that rank+full at n = 2^10 costs 3.8 ns/op on a replayed
sequence but 17.9 ns/op on a fresh sequence of the same workload. The run was
stopped and the harness changed to use a fresh sequence per rep (see
`SequencePool` in `bench/bench.cpp`). The prediction for the re-run: with
`--replay-same-sequence`, small n looks several times faster, and the
difference disappears once one sequence is longer than the predictor can
remember (around n = 2^14).

**H9. Windows power throttling made results depend on variant order.** While
checking H8, rank+full at n = 2^15 measured ~21 ns/op in one configuration
and ~38 ns/op in another. The only real difference was how long naive+none
(which is measured first) had run beforehand. A side test showed that after
~5 s of sustained load, Windows 11 power throttling (EcoQoS) clocked the
pinned core down and kept it there: 18 → 42 ns/op, and it did not recover
after a 3 s pause. With the process opted out
(`SetProcessInformation(ProcessPowerThrottling)`), it stayed at 18–20 ns/op.
The second full run was stopped and the opt-out added to `uf_bench`. The
prediction for the re-run: with `--allow-power-throttling`, measuring
naive+none first makes every later variant at n = 2^15 about 2× slower, and
with the opt-out the order makes no difference.
