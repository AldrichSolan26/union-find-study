"""run_study.py — run the full empirical study and record the machine.

Usage:
    python scripts/run_study.py                 # full study (about an hour)
    python scripts/run_study.py --quick         # small sizes, one seed (~1 min)
    python scripts/run_study.py --resume        # skip chunks already in results/raw/

Writes:
    results/machine.json     CPU, caches of the pinned core, RAM, OS, compiler, settings
    results/raw/*.csv        one CSV per (workload, seed) chunk, so a long run can resume
    results/results.csv      all chunks merged
    results/canary.csv       a fixed small benchmark re-run before every chunk (drift monitor)
    results/validity.csv     measurement-validity experiments (volatile sink; replayed sequences;
                             Windows power throttling)

Needs only the Python standard library; plotting is scripts/plot.py.
"""

import argparse
import csv
import datetime as dt
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The study's fixed configuration. Changing it changes every number in the report.
WORKLOADS = ["random", "chain", "binomial", "local", "local-shuffled", "kruskal"]
FULL = dict(min_exp=10, max_exp=24, slow_max_exp=15, seeds=[1, 2, 3],
            min_reps=3, max_reps=51, min_time_ms=100)
QUICK = dict(min_exp=10, max_exp=16, slow_max_exp=13, seeds=[1],
             min_reps=3, max_reps=11, min_time_ms=20)


def find_binary(name: str) -> Path:
    exe = ".exe" if os.name == "nt" else ""
    for cand in (ROOT / "build" / f"{name}{exe}", ROOT / "build" / "Release" / f"{name}{exe}"):
        if cand.exists():
            return cand
    sys.exit(f"cannot find {name}; build first (see README)")


def run(cmd, **kw) -> str:
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def machine_info(bench: Path, cpu: int) -> dict:
    info = {
        "date": dt.datetime.now().isoformat(timespec="seconds"),
        "os": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "logical_cpus": os.cpu_count(),
        "pinned_cpu": cpu,
    }
    build = run([str(bench), "--build-info", "--cpu", str(cpu)])
    info["build_info"] = [l for l in build.splitlines() if not l.startswith("cache ")]
    info["caches_bytes"] = {l.split()[1]: int(l.split()[2])
                            for l in build.splitlines() if l.startswith("cache ")}
    try:
        if os.name == "nt":
            ps = ("$c=Get-CimInstance Win32_Processor; $s=Get-CimInstance Win32_ComputerSystem;"
                  "\"$($c.Name)|$($c.NumberOfCores)|$($s.TotalPhysicalMemory)\";"
                  "(powercfg /getactivescheme) -replace '.*\\((.*)\\).*','$1'")
            out = run(["powershell", "-NoProfile", "-Command", ps]).strip().splitlines()
            name, cores, ram = out[0].split("|")
            info.update(cpu_model=name.strip(), physical_cores=int(cores),
                        ram_gb=round(int(ram) / 2**30, 1), power_plan=out[-1].strip())
        elif sys.platform == "darwin":
            info["cpu_model"] = run(["sysctl", "-n", "machdep.cpu.brand_string"]).strip()
            info["ram_gb"] = round(int(run(["sysctl", "-n", "hw.memsize"])) / 2**30, 1)
        else:
            for line in Path("/proc/cpuinfo").read_text().splitlines():
                if line.startswith("model name"):
                    info["cpu_model"] = line.split(":", 1)[1].strip()
                    break
            mem = Path("/proc/meminfo").read_text().split()[1]
            info["ram_gb"] = round(int(mem) / 2**20, 1)
    except Exception as exc:  # machine details are nice to have, not essential
        info["probe_error"] = repr(exc)
    return info


def bench_args(cfg: dict, cpu: int) -> list:
    return ["--min-exp", str(cfg["min_exp"]), "--max-exp", str(cfg["max_exp"]),
            "--slow-max-exp", str(cfg["slow_max_exp"]), "--min-reps", str(cfg["min_reps"]),
            "--max-reps", str(cfg["max_reps"]), "--min-time-ms", str(cfg["min_time_ms"]),
            "--cpu", str(cpu)]


def keep_awake() -> None:
    """Stop Windows idle-sleeping during the run. The first full run was split
    by an 11-hour sleep, and the machine came back measurably slower (see
    results/discarded/README.md). Closing the lid still sleeps the machine;
    the canary is what catches that."""
    if os.name == "nt":
        import ctypes
        ES_CONTINUOUS, ES_SYSTEM_REQUIRED = 0x80000000, 0x00000001
        if not ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED):
            print("warning: could not stop the system from sleeping during the run")


def canary(bench: Path, cpu: int, tmp: Path) -> float:
    """A fixed small benchmark (random, n = 2^15, rank+full) used as a drift
    monitor: it is re-run between chunks, so any change in machine speed over
    the hour shows up in results/canary.csv instead of silently in the data."""
    subprocess.run([str(bench), "--workload", "random", "--seeds", "1", "--min-exp", "15", "--max-exp", "15",
                    "--slow-max-exp", "14", "--no-count", "--min-reps", "21", "--max-reps", "21",
                    "--min-time-ms", "0", "--cpu", str(cpu), "--out", str(tmp)],
                   check=True, capture_output=True)
    with tmp.open(newline="") as fh:
        for row in csv.DictReader(fh):
            if row["variant"] == "rank+full":
                return float(row["ns_per_op"])
    raise RuntimeError("canary produced no rank+full row")


def merge(files, dest: Path) -> int:
    rows = 0
    with dest.open("w", newline="") as out:
        writer = None
        for f in files:
            with f.open(newline="") as fh:
                reader = csv.reader(fh)
                header = next(reader)
                if writer is None:
                    writer = csv.writer(out)
                    writer.writerow(header)
                for row in reader:
                    writer.writerow(row)
                    rows += 1
    return rows


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true", help="small sizes, one seed")
    ap.add_argument("--resume", action="store_true", help="keep chunks already in results/raw/")
    ap.add_argument("--no-validity", action="store_true", help="skip the validity experiments")
    ap.add_argument("--cpu", type=int, default=4, help="logical CPU to pin to (-1: no pinning)")
    ap.add_argument("--workloads", default=",".join(WORKLOADS))
    ap.add_argument("--out", default=str(ROOT / "results"))
    args = ap.parse_args()

    cfg = QUICK if args.quick else FULL
    bench, nosink = find_binary("uf_bench"), find_binary("uf_bench_nosink")
    keep_awake()
    out = Path(args.out)
    raw = out / "raw"
    if raw.exists() and not args.resume:
        shutil.rmtree(raw)
    raw.mkdir(parents=True, exist_ok=True)

    info = machine_info(bench, args.cpu)
    info["study_config"] = dict(cfg, workloads=args.workloads.split(","), quick=args.quick)
    if args.resume and (out / "machine.json").exists():
        first = json.loads((out / "machine.json").read_text())
        info["date"] = first.get("date", info["date"])          # when the study started
        info["resumed"] = first.get("resumed", []) + [dt.datetime.now().isoformat(timespec="seconds")]
    (out / "machine.json").write_text(json.dumps(info, indent=2))
    print(f"machine: {info.get('cpu_model', '?')}, caches {info['caches_bytes']}")

    chunks = []
    canary_rows = []
    canary_file = out / "canary.csv"
    if args.resume and canary_file.exists():
        with canary_file.open(newline="") as fh:
            canary_rows = list(csv.DictReader(fh))

    def record_canary(before: str) -> None:
        ns = canary(bench, args.cpu, raw / "canary.tmp.csv")
        canary_rows.append({"time": dt.datetime.now().isoformat(timespec="seconds"), "before": before,
                            "rank_full_random_2e15_ns_per_op": f"{ns:.3f}"})
        with canary_file.open("w", newline="") as fh:
            writer = csv.DictWriter(fh, fieldnames=list(canary_rows[0].keys()))
            writer.writeheader()
            writer.writerows(canary_rows)
        print(f"canary before {before}: {ns:.2f} ns/op", flush=True)
        values = sorted(float(r["rank_full_random_2e15_ns_per_op"]) for r in canary_rows)
        typical = values[len(values) // 2]
        if abs(ns / typical - 1) > 0.10:
            print(f"WARNING: canary {ns:.2f} ns/op is {ns / typical - 1:+.0%} from the run's median "
                  f"({typical:.2f}); the machine state has changed. Check before trusting the next chunk.",
                  flush=True)

    for w in args.workloads.split(","):
        for seed in cfg["seeds"]:
            dest = raw / f"{w}_seed{seed}.csv"
            chunks.append(dest)
            if args.resume and dest.exists():
                print(f"skip {dest.name} (exists)")
                continue
            record_canary(dest.stem)
            tmp = dest.with_suffix(".partial")
            cmd = [str(bench), "--workload", w, "--seeds", str(seed), "--out", str(tmp)] + bench_args(cfg, args.cpu)
            print("run", " ".join(cmd[1:]), flush=True)
            subprocess.run(cmd, check=True)        # non-zero on any checksum mismatch
            tmp.replace(dest)                      # only complete chunks count
    record_canary("end")

    rows = merge(chunks, out / "results.csv")
    print(f"wrote {out / 'results.csv'} ({rows} rows)")
    if args.no_validity:
        return

    # Validity experiments. H7: the chain benchmark with the volatile sink on
    # vs off. H8: the random benchmark with a fresh sequence per timed rep vs
    # one sequence replayed (lets the branch predictor learn it).
    # H9 (Windows only): random at n = 2^15 with naive+none measured first,
    # power throttling disabled (as in the study) vs left on.
    sizes = {"sink": ["--min-exp", "10", "--max-exp", "15"], "replay": ["--min-exp", "10", "--max-exp", "16"],
             "throttle": ["--min-exp", "15", "--max-exp", "15"]}
    experiments = [
        ("sink", "sink", bench, "chain", []),
        ("sink", "nosink", nosink, "chain", []),
        ("replay", "fresh", bench, "random", []),
        ("replay", "replayed", bench, "random", ["--replay-same-sequence"]),
    ]
    if os.name == "nt":
        experiments += [("throttle", "disabled", bench, "random", []),
                        ("throttle", "allowed", bench, "random", ["--allow-power-throttling"])]
    parts = []
    for experiment, mode, exe, workload, extra in experiments:
        tmp = raw / f"validity_{experiment}_{mode}.csv"
        subprocess.run([str(exe), "--workload", workload, "--seeds", "1", "--out", str(tmp), "--no-count",
                        "--slow-max-exp", "15", "--min-reps", "21", "--max-reps", "51",
                        "--min-time-ms", "50", "--cpu", str(args.cpu)] + sizes[experiment] + extra, check=True)
        with tmp.open(newline="") as fh:
            for row in csv.DictReader(fh):
                parts.append(dict(experiment=experiment, mode=mode, **row))
    with (out / "validity.csv").open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(parts[0].keys()))
        writer.writeheader()
        writer.writerows(parts)
    print(f"wrote {out / 'validity.csv'}")


if __name__ == "__main__":
    main()
