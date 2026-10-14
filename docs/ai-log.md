# AI use log

Add an entry whenever you use an AI tool, and especially whenever its output
was wrong, misleading or needed fixing. Report section (d) needs at least two
concrete "AI was wrong" cases, and this log is where they come from.

| Date | Tool | What I asked for | What it produced | Was it right? How did I check? | What I did |
| --- | --- | --- | --- | --- | --- |
| 2026-10-14 | Claude Code (Opus 5.5) | Check the repo against the PA1 rubric, then rebuild it so every item is satisfied (report and video excluded) | Rewrote the library, tests, harness, runner and plots; ran the study; wrote README, hypotheses and findings | See the session record below; the mistakes it made are listed there | |

## Session record: Claude Code, 2026-10-14

A factual record of what the AI did in this session, so the AI-use section
can be written from evidence rather than memory.

The session ran over two sittings on 2026-10-14. The second sitting
finished the study, wrote `results/FINDINGS.md` and added `experiments/`.

**What it changed:**

- **Library:** added a zero-cost `Stats` policy that counts parent-array reads
  and writes; replaced `assert`s with exceptions for out-of-range arguments and
  negative n; allowed n = 0.
- **Workloads:**
  - added `binomial` (worst case for rank/size), and `local` /
    `local-shuffled` (a locality experiment with identical work);
  - fixed the Kruskal ns/op denominator, which divided by all edges, not the
    edges actually examined;
  - replaced `std::uniform_int_distribution` and `std::shuffle`, whose output
    differs between standard libraries, so workloads are identical on every
    platform.
- **Tests:** went from 5,919 to 7,315 checks. New checks cover exact
  compression shapes, the tight log2 n bound, the counters, the
  local/shuffled equivalence, and Kruskal against Prim's algorithm. The tests
  were validated by injecting 8 bugs; all 8 were caught.
- **Harness:**
  - pins to a performance core;
  - reports median, quartiles and minimum;
  - repeats over 3 seeds;
  - adds a drift-monitor canary;
  - adds the two safeguards below.

**Where the AI was wrong or misleading in this session.** These were caught
by checking the data, not by reading the code.

1. **Its rebuilt harness replayed the same op sequence for every timed rep.**
   This was inherited from the original harness, and the AI did not question
   it. The first full run showed rank+full getting ~3× slower between
   n = 2^10 and 2^12, though both fit in L1, so caching could not explain it.
   A side test showed the branch predictor had memorised the replayed
   sequence: 3.8 ns/op replayed against 17.9 ns/op on a fresh sequence. The
   run was stopped, and the harness now uses fresh sequences
   (`SequencePool`). Reproduce it with `--replay-same-sequence`.
2. **Its check for background throttling gave a false "all clear".** It ran
   the same quick benchmark in the foreground and in the background, found
   them equal (median ratio 0.998), and concluded that throttling was not a
   problem. But both runs were throttled in the same way, so the comparison
   could not detect it. The real problem surfaced later as an order effect:
   rank+full at n = 2^15 measured 21 or 38 ns/op depending on how long
   naive+none had run first. The cause was Windows 11 power throttling
   (EcoQoS), which slowed the core ~2.3× after ~5 s of load. The fix is the
   `SetProcessInformation(ProcessPowerThrottling)` opt-out. Reproduce it with
   `--allow-power-throttling`.
3. **It reintroduced the dead-code bug it had just documented (H7).**
   `experiments/common.hpp`, its helper for the side experiments, discarded
   the checksum that `run_ops` returns. The compiler was then free to delete
   the side-effect-free `connected()` queries, and rank+none came out at 14.4
   ns/op instead of the 22.7 that `uf_bench` measures. It even reversed the
   ranking of rank+none and rank+full. The bug was caught only because the
   two programs disagreed. Ruling out variant order, heap layout and code
   alignment, one experiment at a time, eventually pointed to the discarded
   return value. Fixed with a `volatile` sink. The H8 and H9 numbers in the
   findings come from `uf_bench`, so they were not affected.
4. **Its drift monitor had a blind spot that it did not notice at first.** The
   canary runs at n = 2^15, which fits in L2. It detects clock-speed changes
   but not memory-bandwidth contention. One chunk (`local` seed 2) was 13–25%
   slow at n = 2^19–2^22 with a normal canary. It was caught because the
   seeds disagreed, and was re-measured. The limitation is now stated in
   `summary.md` and `FINDINGS.md`.
5. **It did not plan for the laptop sleeping.** The first full run was split by
   an 11-hour sleep, and three chunks measured around it were ~25% slow. The
   runner now prevents idle sleep and warns on canary drift. The affected data
   is in `results/discarded/`.
6. **Overclaims in its own findings draft, caught when it checked each
   number against the data before committing:**
   - "at 2^24 the compression spread shrinks to 2–12% everywhere" was false,
     because `binomial` stays at 22–25%;
   - "full compression is never fastest by a meaningful margin" became "6 of
     180 configurations, never by more than 5%";
   - it also corrected three smaller ranges.
7. **Smaller slips:**
   - a function was used before it was defined, which broke the build (caught
     by the compiler);
   - a `sed` edit turned a `\n` escape into a real newline (caught by the
     compiler);
   - it wrote a meaningless comment into `run_study.py` and then removed it;
   - it gave two log files misleading names and then reorganised them into
     `results/logs/`.

**Claims it verified rather than assumed:**

- The original `bench.cpp` comment said that without the `volatile` sink,
  naive+none on `chain` looks fastest. Measured: 2.2 ns/op without the sink
  against 8,530 ns/op with it, at n = 2^14.
- `local` and `local-shuffled` do identical work: tested exactly, counter for
  counter.
- The measured path lengths match closed-form expectations for `chain` and
  `binomial` (`results/figures/theory.png`).
- Workloads are identical across standard libraries: a clang build with
  Microsoft's standard library reproduces all 1,080 compared rows exactly
  (checksums, op counts, reads, writes, path lengths, depths).
- The CMake build works as the README describes. It was checked with a
  temporary CMake 4.4 and the MinGW Makefiles generator; it defaults to
  Release and passes CTest.

**Where the AI's explanations are inferences, not measurements** (also
labelled in `FINDINGS.md`):
- that cache misses cause the time growth in H2;
- that branch mispredictions dominate the small-n cost;
- why halving's extra advantage shrinks at large n;
- why rank beats size on `kruskal` but not on `random`. This one is not
  explained at all.

No hardware performance counters were used.

## Things I took on trust (revisit before submitting)

- Fill in from your own experience: which parts of the code or analysis you
  could not fully verify yourself.
