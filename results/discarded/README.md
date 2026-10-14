# Discarded measurements

These chunks were measured during the study but are **not used** in
`results.csv`, the figures or the findings. They are kept for transparency,
and as a between-session reproducibility check.

Every chunk in `results.csv` was measured on the evening of 2026-10-14
(20:47–22:14). The drift-monitor canary stayed between 20.60 and 20.77 ns/op
(`../canary.csv`), and the logs of those runs are in `../logs/run2…run5`.

## 1. The overnight sleep (run 1)

The first full run started at 02:10 on 2026-10-14. The `local` seed 2 chunk
started at 02:43 and normally takes about 3 minutes, but did not finish until
16:43. The Windows event log shows the system asleep from 05:44 to 16:41, and
the process apparently made no progress before that either. After waking,
the canary read **26.4 ns/op**, about 25% above its earlier 20.6–23.5.

| File | Why it was discarded |
| --- | --- |
| `local_seed2.csv` | the chunk spanned the sleep |
| `local_seed3.csv` | measured right after waking, canary 26.4 ns/op |
| `local-shuffled_seed1.incomplete.csv` | measured after waking, canary 26.5; cut off at n = 2^23 when the session ended |
| `canary_before_resume.csv` | the full canary history of run 1 (log: `../logs/run1_initial.log`) |

**Since then:** `run_study.py` keeps Windows from idle-sleeping for the whole
run, and it warns whenever a canary is more than 10% from the run's median.

## 2. The rest of run 1 (`session1/`)

The ten run-1 chunks that finished before the sleep were valid in isolation,
but run 1's canary was noisier (20.6–23.5 ns/op) than the evening session's
(20.60–20.77). Its `random` seed 2 chunk ran at a canary of 23.5, and random
seed 1 at n = 2^15 was ~30% faster across every variant, including
naive+none. That looks like a temporary clock-speed change, not anything the
algorithm did.

So all ten chunks were re-measured in the evening session, so that the whole
dataset comes from one stable session. The old versions are in `session1/`,
with their canary readings in `session1/canary_session1.csv`.

**Reproducibility check (random workload, run 1 vs the final data):**
- **work:** identical, with the same checksums, op counts, reads, writes and
  path lengths on every row;
- **time:** median ratio 1.02, 10th percentile 1.00, 90th percentile 1.16.

## 3. A transient slowdown (`transient/`)

`local` seed 2 from run 2 (`transient/local_seed2_run2.csv`) was 13–25%
slower than seeds 1 and 3 at n = 2^19–2^22. Its work was identical, and the
canary was normal (20.68).

Re-measured in run 5, it matched the other seeds to within 1% at every n. So
it was transient: most likely memory-bandwidth contention from another program
during that chunk. The canary cannot detect that kind of slowdown, because it
runs at n = 2^15, which fits in L2.
