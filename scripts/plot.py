"""plot.py — turn the study's CSVs into figures and a summary.

Usage:
    python scripts/plot.py [results/]          # reads results.csv, validity.csv, machine.json

Writes into results/:
    figures/<workload>.png   time (top row) and work (bottom row), one panel per link policy
    figures/theory.png       measured path lengths against the textbook predictions
    figures/time_vs_work.png flat work, rising time: cost per memory access by n
    figures/locality.png     local vs local-shuffled (identical work, different addresses)
    figures/validity.png     the two measurement pitfalls the harness guards against
    summary.md               tables and the key numbers each hypothesis is judged on

Every point is the median over seeds of each seed's median run time; shaded
bands span the per-seed minimum to maximum. Requires pandas and matplotlib.
"""

import argparse
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

# Palette: the find policy is the identity being compared. "none" is the
# baseline, drawn in gray; the three compression policies take the first
# three categorical slots (validated colour-blind safe as a set). Line style
# is a second, colour-independent encoding.
SURFACE, INK, INK2, MUTED, GRID, AXIS = "#fcfcfb", "#0b0b0b", "#52514e", "#898781", "#e1e0d9", "#c3c2b7"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a"]
FIND_ORDER = ["none", "full", "halving", "splitting"]
FIND_COLOUR = {"none": MUTED, "full": SERIES[0], "halving": SERIES[1], "splitting": SERIES[2]}
FIND_STYLE = {"none": (0, (1, 1.5)), "full": "-", "halving": (0, (5, 2)), "splitting": (0, (3, 1.5, 1, 1.5))}
LINKS = ["naive", "rank", "size"]
BYTES_PER_ELEMENT = {"naive": 4, "rank": 5, "size": 8}   # parent int + rank byte / size int
BALANCED_COMPRESSED = [f"{l}+{f}" for l in ("rank", "size") for f in ("full", "halving", "splitting")]

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "font.family": ["Segoe UI", "DejaVu Sans", "sans-serif"], "font.size": 9,
    "axes.edgecolor": AXIS, "axes.labelcolor": INK2, "axes.titlecolor": INK,
    "axes.titlesize": 10, "axes.titleweight": "bold", "axes.grid": True,
    "grid.color": GRID, "grid.linewidth": 0.6, "grid.linestyle": "-",
    "xtick.color": INK2, "ytick.color": INK2, "legend.frameon": False,
    "lines.linewidth": 1.6, "lines.markersize": 3.5,
})


def aggregate(df: pd.DataFrame) -> pd.DataFrame:
    """One row per (workload, variant, n): median over seeds plus seed range."""
    g = df.groupby(["workload", "variant", "link", "find", "n"], as_index=False)
    return g.agg(ns=("ns_per_op", "median"), ns_lo=("ns_per_op", "min"), ns_hi=("ns_per_op", "max"),
                 reads=("reads_per_op", "mean"), writes=("writes_per_op", "mean"),
                 path=("path_per_find", "mean"), max_depth=("max_depth", "max"),
                 seeds=("seed", "nunique"))


def log2_axis(ax) -> None:
    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"$2^{{{int(round(np.log2(v)))}}}$"))
    ax.xaxis.set_major_locator(matplotlib.ticker.LogLocator(base=2, numticks=20))
    ax.xaxis.set_minor_locator(matplotlib.ticker.NullLocator())
    ax.tick_params(length=0)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)


def log_y(ax) -> None:
    """Log y-axis with plain-number ticks; denser ticks when the range is small."""
    ax.set_yscale("log")
    lo, hi = ax.get_ylim()
    subs = (1.0, 2.0, 5.0) if hi / lo > 8 else (1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0)
    ax.yaxis.set_major_locator(matplotlib.ticker.LogLocator(base=10, subs=subs))
    ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g}"))
    ax.yaxis.set_minor_locator(matplotlib.ticker.NullLocator())
    ax.set_ylim(lo, hi)


def cache_lines(ax, caches: dict, bytes_per_element: int, label: bool) -> None:
    """Mark the n at which the main arrays fill each cache level."""
    lo, hi = ax.get_xlim()
    for level in ("L1", "L2", "L3"):
        if level not in caches:
            continue
        n = caches[level] / bytes_per_element
        if lo <= n <= hi:
            ax.axvline(n, color=AXIS, linewidth=0.8, zorder=0)
            if label:
                ax.text(n, 1.0, f" {level}", transform=ax.get_xaxis_transform(), color=MUTED,
                        fontsize=7.5, va="top", ha="left")


def plot_series(ax, sub: pd.DataFrame, y: str, colour, style, label, band=True, marker="o") -> None:
    sub = sub.sort_values("n")
    if band and y == "ns":
        ax.fill_between(sub["n"], sub["ns_lo"], sub["ns_hi"], color=colour, alpha=0.15, linewidth=0)
    ax.plot(sub["n"], sub[y], color=colour, linestyle=style, marker=marker, label=label)


def workload_figure(agg: pd.DataFrame, workload: str, caches: dict, out: Path) -> None:
    data = agg[agg.workload == workload]
    fig, axes = plt.subplots(2, 3, figsize=(11, 6.2), sharex=True)
    # rank and size share y-scales so they compare directly; naive has its own
    # (naive+none is orders of magnitude slower on adversarial inputs).
    for row in range(2):
        axes[row, 2].sharey(axes[row, 1])
    for col, link in enumerate(LINKS):
        for row, (y, ylabel) in enumerate((("ns", "time per operation (ns)"),
                                           ("reads", "parent-array reads per operation"))):
            ax = axes[row, col]
            for find in FIND_ORDER:
                sub = data[(data.link == link) & (data["find"] == find)]
                if len(sub):
                    plot_series(ax, sub, y, FIND_COLOUR[find], FIND_STYLE[find], find)
            log2_axis(ax)
            if row == 0:
                ax.set_title(f"{link} linking")
            if col == 0:
                ax.set_ylabel(ylabel)
            if row == 1:
                ax.set_xlabel("n (elements)")
    for row in range(2):              # after all data is drawn, so shared limits are final
        for col in range(3):
            log_y(axes[row, col])
    for col, link in enumerate(LINKS):
        for row in range(2):
            cache_lines(axes[row, col], caches, BYTES_PER_ELEMENT[link], label=row == 0)
    handles, labels = axes[0, 1].get_legend_handles_labels()
    fig.legend(handles, labels, title="find policy", ncol=4, loc="lower center", bbox_to_anchor=(0.5, -0.01))
    fig.suptitle(f"{workload} workload", x=0.01, ha="left", fontweight="bold", color=INK, fontsize=12)
    note = "naive+none omitted above its size cap" if (data.variant == "naive+none").any() and \
        data[data.variant == "naive+none"].n.max() < data.n.max() else ""
    fig.text(0.99, 0.985, ("vertical lines: n at which the arrays fill L1 / L2 / L3 of the pinned core.  " + note).strip(),
             ha="right", va="top", color=MUTED, fontsize=7.5)
    fig.tight_layout(rect=(0, 0.06, 1, 0.97))
    fig.savefig(out / f"{workload}.png", dpi=150)
    plt.close(fig)


def theory_figure(agg: pd.DataFrame, out: Path) -> None:
    fig, axes = plt.subplots(1, 3, figsize=(11, 3.6))

    # (a) chain + naive+none: n-1 unites find two roots (depth 0); n finds hit a
    # uniform node of a path, mean depth (n-1)/2.
    ax = axes[0]
    sub = agg[(agg.workload == "chain") & (agg.variant == "naive+none")].sort_values("n")
    n = sub["n"].to_numpy(dtype=float)
    ax.plot(n, n * (n - 1) / 2 / (3 * n - 2), color=INK, linewidth=0.9, label="theory: n(n−1)/2 ÷ (3n−2)")
    ax.plot(sub["n"], sub["path"], color=FIND_COLOUR["none"], marker="o", linestyle="none", markersize=5,
            label="measured, naive+none")
    log_y(ax)
    ax.set_title("chain: naive+none is Θ(n) per find")
    ax.set_ylabel("mean path length per find")

    # (b) binomial + rank/size+none: n finds hit a uniform node of a binomial
    # tree B_k, mean depth k/2; the 2(n-1) finds inside unites hit roots.
    ax = axes[1]
    sub = agg[(agg.workload == "binomial") & (agg["find"] == "none") & (agg.link != "naive")]
    n = np.sort(sub["n"].unique()).astype(float)
    ax.plot(n, n * np.log2(n) / 2 / (3 * n - 2), color=INK, linewidth=0.9, label="theory: n·log₂n/2 ÷ (3n−2)")
    # rank and size build the same trees here (every merge is a tie), so the
    # size marker is drawn larger and hollow to keep both visible.
    for link, colour, mk, size, face in (("rank", SERIES[0], "o", 4.5, SERIES[0]), ("size", SERIES[1], "s", 8, "none")):
        s = sub[sub.link == link].sort_values("n")
        ax.plot(s["n"], s["path"], color=colour, marker=mk, linestyle="none", markersize=size,
                markerfacecolor=face, markeredgewidth=1.3, label=f"measured, {link}+none")
    ax.set_title("binomial: rank/size+none is Θ(log n)")

    # (c) random: compression keeps paths short at every n.
    ax = axes[2]
    for variant, colour, style in (("rank+full", SERIES[0], "-"), ("rank+halving", SERIES[1], (0, (5, 2))),
                                   ("naive+full", MUTED, (0, (1, 1.5)))):
        s = agg[(agg.workload == "random") & (agg.variant == variant)].sort_values("n")
        ax.plot(s["n"], s["path"], color=colour, linestyle=style, marker="o", label=variant)
    ax.set_title("random: compression keeps paths short")
    ax.set_ylim(bottom=0)
    for ax in axes:
        log2_axis(ax)
        ax.set_xlabel("n (elements)")
        ax.legend(fontsize=7.5, loc="upper left")
    fig.tight_layout()
    fig.savefig(out / "theory.png", dpi=150)
    plt.close(fig)


def time_vs_work_figure(agg: pd.DataFrame, caches: dict, out: Path) -> None:
    rnd = agg[agg.workload == "random"]
    fig, axes = plt.subplots(1, 2, figsize=(11, 4))

    # (a) one variant, both measures indexed to their value at the smallest n.
    ax = axes[0]
    s = rnd[rnd.variant == "rank+halving"].sort_values("n")
    base = s.iloc[0]
    ax.plot(s["n"], s["ns"] / base["ns"], color=SERIES[0], marker="o", label="time per op")
    ax.plot(s["n"], s["reads"] / base["reads"], color=SERIES[1], marker="s", linestyle=(0, (5, 2)),
            label="reads per op")
    ax.axhline(1, color=AXIS, linewidth=0.8)
    for _, row in s.iloc[[-1]].iterrows():
        ax.annotate(f"{row['ns'] / base['ns']:.1f}×", (row["n"], row["ns"] / base["ns"]),
                    textcoords="offset points", xytext=(-6, -10), ha="right", va="top", color=INK2)
    log2_axis(ax)
    ax.set_title(f"random, rank+halving: indexed to n = 2^{int(np.log2(base['n']))}")
    ax.set_ylabel("relative to smallest n (= 1)")
    ax.set_xlabel("n (elements)")
    ax.legend(loc="upper left")
    cache_lines(ax, caches, BYTES_PER_ELEMENT["rank"], label=True)

    # (b) ns per parent-array read, for every balanced + compressed variant.
    ax = axes[1]
    for variant in BALANCED_COMPRESSED:
        link, find = variant.split("+")
        s = rnd[rnd.variant == variant].sort_values("n")
        ax.plot(s["n"], s["ns"] / s["reads"], color=FIND_COLOUR[find],
                linestyle="-" if link == "rank" else (0, (5, 2)),
                marker="o" if link == "rank" else "s", label=variant)
    log2_axis(ax)
    ax.set_title("random: nanoseconds per parent-array read")
    ax.set_ylabel("ns per read")
    ax.set_xlabel("n (elements)")
    ax.set_ylim(bottom=0)
    ax.legend(ncol=2, fontsize=7.5, loc="upper left")
    cache_lines(ax, caches, BYTES_PER_ELEMENT["rank"], label=True)
    fig.tight_layout()
    fig.savefig(out / "time_vs_work.png", dpi=150)
    plt.close(fig)


def locality_figure(agg: pd.DataFrame, caches: dict, out: Path) -> None:
    fig, axes = plt.subplots(1, 2, figsize=(11, 4))
    ax = axes[0]
    for wl, colour, style, mk in (("local", SERIES[0], "-", "o"), ("local-shuffled", SERIES[1], (0, (5, 2)), "s")):
        s = agg[(agg.workload == wl) & (agg.variant == "rank+halving")]
        plot_series(ax, s, "ns", colour, style, wl, marker=mk)
    log2_axis(ax)
    log_y(ax)
    ax.set_title("rank+halving: same operations, relabelled ids")
    ax.set_ylabel("time per operation (ns)")
    ax.set_xlabel("n (elements)")
    ax.legend(loc="upper left")
    cache_lines(ax, caches, BYTES_PER_ELEMENT["rank"], label=True)

    ax = axes[1]
    loc = agg[agg.workload == "local"].set_index(["variant", "n"])
    shf = agg[agg.workload == "local-shuffled"].set_index(["variant", "n"])
    ratio = (shf["ns"] / loc["ns"]).rename("ratio").reset_index()
    ratio = ratio[ratio.variant.isin(BALANCED_COMPRESSED)]
    stats = ratio.groupby("n").ratio.agg(["median", "min", "max"]).reset_index()
    ax.fill_between(stats["n"], stats["min"], stats["max"], color=SERIES[0], alpha=0.15, linewidth=0,
                    label="range over the 6 rank/size + compression variants")
    ax.plot(stats["n"], stats["median"], color=SERIES[0], marker="o", label="median")
    ax.axhline(1, color=AXIS, linewidth=0.8)
    log2_axis(ax)
    ax.set_title("slowdown from scattering the same work")
    ax.set_ylabel("local-shuffled ÷ local (time)")
    ax.set_xlabel("n (elements)")
    ax.legend(loc="upper left", fontsize=7.5)
    cache_lines(ax, caches, BYTES_PER_ELEMENT["rank"], label=True)
    fig.tight_layout()
    fig.savefig(out / "locality.png", dpi=150)
    plt.close(fig)


def validity_figure(validity: pd.DataFrame, out: Path) -> None:
    throttle = validity[validity.experiment == "throttle"]
    panels = 3 if len(throttle) else 2
    fig, axes = plt.subplots(1, panels, figsize=(5.5 * panels, 3.8))
    ax = axes[0]
    s = validity[(validity.experiment == "sink") & (validity.variant == "naive+none")]
    for mode, colour, style, mk, label in (("sink", SERIES[0], "-", "o", "with volatile sink (correct)"),
                                           ("nosink", SERIES[1], (0, (5, 2)), "s", "sink removed")):
        m = s[s["mode"] == mode].sort_values("n")
        ax.plot(m["n"], m["ns_per_op"], color=colour, linestyle=style, marker=mk, label=label)
    log2_axis(ax)
    log_y(ax)
    ax.set_title("chain, naive+none: without the sink the finds vanish")
    ax.set_ylabel("time per operation (ns)")
    ax.set_xlabel("n (elements)")
    ax.legend(loc="center right")

    ax = axes[1]
    r = validity[(validity.experiment == "replay") & (validity.variant == "rank+full")]
    for mode, colour, style, mk, label in (("fresh", SERIES[0], "-", "o", "fresh sequence every rep (correct)"),
                                           ("replayed", SERIES[1], (0, (5, 2)), "s", "one sequence replayed")):
        m = r[r["mode"] == mode].sort_values("n")
        ax.plot(m["n"], m["ns_per_op"], color=colour, linestyle=style, marker=mk, label=label)
    log2_axis(ax)
    ax.set_ylim(bottom=0)
    ax.set_title("random, rank+full: a replayed sequence is learned")
    ax.set_ylabel("time per operation (ns)")
    ax.set_xlabel("n (elements)")
    ax.legend(loc="lower right")

    if panels == 3:
        # Every variant measured after naive+none, at one n, in harness order.
        ax = axes[2]
        order = [v for v in throttle[throttle["mode"] == "disabled"].variant if v != "naive+none"]
        x = np.arange(len(order))
        for mode, colour, mk, label in (("disabled", SERIES[0], "o", "throttling disabled (study setting)"),
                                        ("allowed", SERIES[1], "s", "throttling left on")):
            m = throttle[throttle["mode"] == mode].set_index("variant").reindex(order)
            ax.plot(x, m["ns_per_op"], color=colour, marker=mk, linestyle="none", markersize=6, label=label)
        ax.set_xticks(x, [v.replace("+", "+\n") for v in order], fontsize=7)
        ax.tick_params(length=0)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        ax.set_ylim(bottom=0)
        n = int(throttle.n.iloc[0])
        ax.set_title(f"random, n = 2^{int(np.log2(n))}, after naive+none ran first")
        ax.set_ylabel("time per operation (ns)")
        ax.legend(loc="lower right", fontsize=7.5)
    fig.tight_layout()
    fig.savefig(out / "validity.png", dpi=150)
    plt.close(fig)


def md_table(df: pd.DataFrame) -> str:
    cols = list(df.columns)
    lines = ["| " + " | ".join(cols) + " |", "| " + " | ".join("---" for _ in cols) + " |"]
    for _, row in df.iterrows():
        lines.append("| " + " | ".join(str(row[c]) for c in cols) + " |")
    return "\n".join(lines)


def loglog_slope(s: pd.DataFrame, y: str, n_min: int) -> float:
    s = s[(s.n >= n_min) & (s[y] > 0)]
    if len(s) < 3:
        return float("nan")
    return float(np.polyfit(np.log2(s["n"]), np.log2(s[y]), 1)[0])


def summary(df: pd.DataFrame, agg: pd.DataFrame, validity, machine: dict, out: Path) -> str:
    L = []
    nmax = int(agg.n.max())
    e_max = int(np.log2(nmax))
    L.append("# Results summary\n")
    L.append("Generated by `scripts/plot.py` from `results.csv`. Do not edit by hand.\n")
    L.append(f"- Machine: {machine.get('cpu_model', '?')}, pinned to logical CPU {machine.get('pinned_cpu')}; "
             f"caches of that core: " + ", ".join(f"{k} {v / 2**20:g} MB" if v >= 2**20 else f"{k} {v / 2**10:g} KB"
                                                   for k, v in sorted(machine.get("caches_bytes", {}).items())))
    L.append(f"- Build: {'; '.join(machine.get('build_info', []))}")
    L.append(f"- OS: {machine.get('os')}; power plan: {machine.get('power_plan', '?')}; run started {machine.get('date')}")
    cfg = machine.get("study_config", {})
    L.append(f"- Sweep: n = 2^{cfg.get('min_exp')} … 2^{cfg.get('max_exp')}, seeds {cfg.get('seeds')}, "
             f"≥{cfg.get('min_reps')} timed reps and ≥{cfg.get('min_time_ms')} ms per point (max {cfg.get('max_reps')} reps); "
             f"naive+none capped at 2^{cfg.get('slow_max_exp')}")

    # Correctness of the measurement itself.
    agree = df.groupby(["workload", "n", "seed"]).checksum.nunique().max() == 1
    counts_stable = df.groupby(["workload", "variant", "n"]).reads_per_op.nunique().max()
    spread = (agg.ns_hi - agg.ns_lo) / agg.ns
    L.append(f"- Rows: {len(df)}; all variants agree on every checksum: **{'yes' if agree else 'NO'}**")
    L.append(f"- Seed-to-seed spread of ns/op ((max−min)/median over seeds): median {spread.median():.1%}, "
             f"90th percentile {spread.quantile(0.9):.1%}, worst {spread.max():.1%}")
    within = (df.p75_ns - df.p25_ns) / df.median_ns
    L.append(f"- Within-seed spread (IQR/median of the timed reps): median {within.median():.1%}, "
             f"90th percentile {within.quantile(0.9):.1%}")
    if (out / "canary.csv").exists():
        canary_df = pd.read_csv(out / "canary.csv")
        c = canary_df["rank_full_random_2e15_ns_per_op"]
        chunks = int((canary_df["before"] != "end").sum())
        L.append(f"- Drift monitor (`canary.csv`: random, n = 2^15, rank+full, run before each of the {chunks} "
                 f"chunks and at the end of each run, {len(c)} readings): {c.min():.2f}–{c.max():.2f} ns/op, "
                 f"max/min = {c.max() / c.min():.3f}. It fits in L2, so it detects clock-speed changes but "
                 f"not memory-bandwidth contention.")
    L.append("")

    # Key numbers per hypothesis.
    L.append("## Key numbers for each hypothesis\n")
    def val(w, v, n, col="ns"):
        r = agg[(agg.workload == w) & (agg.variant == v) & (agg.n == n)]
        return float(r[col].iloc[0]) if len(r) else float("nan")

    rows = []
    for w, v in (("chain", "naive+none"), ("binomial", "rank+none"), ("binomial", "size+none"),
                 ("random", "naive+full"), ("random", "naive+halving"), ("random", "rank+full"),
                 ("random", "rank+halving"), ("kruskal", "rank+full"), ("kruskal", "naive+full")):
        s = agg[(agg.workload == w) & (agg.variant == v)]
        rows.append({"workload": w, "variant": v,
                     "slope of log2(path/find) vs log2 n, n ≥ 2^14": f"{loglog_slope(s, 'path', 2**14):.3f}",
                     "path/find at smallest n": f"{s.sort_values('n').path.iloc[0]:.3f}",
                     "path/find at largest n": f"{s.sort_values('n').path.iloc[-1]:.3f}",
                     "largest n": f"2^{int(np.log2(s.n.max()))}"})
    L.append("**H1 (work follows theory).** A slope of 1 means linear growth, 0 means flat. "
             "Θ(log n) shows up as a slope that shrinks towards 0 as n grows.\n")
    L.append(md_table(pd.DataFrame(rows)) + "\n")

    n0 = int(agg.n.min())
    rows = []
    for v in BALANCED_COMPRESSED:
        rows.append({"variant": v, f"ns/op 2^{int(np.log2(n0))}": f"{val('random', v, n0):.2f}",
                     f"ns/op 2^{e_max}": f"{val('random', v, nmax):.2f}",
                     "time ratio": f"{val('random', v, nmax) / val('random', v, n0):.2f}×",
                     "reads ratio": f"{val('random', v, nmax, 'reads') / val('random', v, n0, 'reads'):.2f}×",
                     f"ns/read 2^{e_max}": f"{val('random', v, nmax) / val('random', v, nmax, 'reads'):.2f}"})
    L.append(f"**H2 (time does not follow theory), random workload, 2^{int(np.log2(n0))} → 2^{e_max}.**\n")
    L.append(md_table(pd.DataFrame(rows)) + "\n")

    rows = []
    for n in sorted(agg.n.unique()):
        r = [val("local-shuffled", v, n) / val("local", v, n) for v in BALANCED_COMPRESSED]
        same = all(abs(val("local-shuffled", v, n, "reads") - val("local", v, n, "reads")) < 1e-9
                   for v in BALANCED_COMPRESSED)
        rows.append({"n": f"2^{int(np.log2(n))}", "shuffled ÷ local, median": f"{np.median(r):.2f}",
                     "min": f"{min(r):.2f}", "max": f"{max(r):.2f}", "reads identical": "yes" if same else "NO"})
    L.append("**H3 (locality).** Time ratio local-shuffled ÷ local over the 6 rank/size + compression variants.\n")
    L.append(md_table(pd.DataFrame(rows)) + "\n")

    rows = []
    for w in ("random", "kruskal", "local-shuffled"):
        for n in (n0, 2**16, 2**20, nmax):
            for link in ("rank", "size"):
                t = {f: val(w, f"{link}+{f}", n) for f in ("none", "full", "halving", "splitting")}
                comp = [t["full"], t["halving"], t["splitting"]]
                rows.append({"workload": w, "n": f"2^{int(np.log2(n))}", "link": link,
                             "full": f"{t['full']:.2f}", "halving": f"{t['halving']:.2f}",
                             "splitting": f"{t['splitting']:.2f}", "none": f"{t['none']:.2f}",
                             "spread among compression": f"{max(comp) / min(comp) - 1:.0%}",
                             "none ÷ best compression": f"{t['none'] / min(comp):.2f}×",
                             "fastest": min(("full", "halving", "splitting"), key=lambda f: t[f])})
    L.append("**H4 (which compression).** ns/op.\n")
    L.append(md_table(pd.DataFrame(rows)) + "\n")

    rows = []
    for w in ("random", "kruskal", "local-shuffled", "binomial"):
        for n in (n0, 2**16, 2**20, nmax):
            for f in ("full", "halving", "splitting"):
                rows.append({"workload": w, "n": f"2^{int(np.log2(n))}", "find": f,
                             "rank ns/op": f"{val(w, f'rank+{f}', n):.2f}", "size ns/op": f"{val(w, f'size+{f}', n):.2f}",
                             "size ÷ rank (time)": f"{val(w, f'size+{f}', n) / val(w, f'rank+{f}', n):.2f}",
                             "size ÷ rank (reads)": f"{val(w, f'size+{f}', n, 'reads') / val(w, f'rank+{f}', n, 'reads'):.3f}"})
    L.append("**H5 (rank vs size).**\n")
    L.append(md_table(pd.DataFrame(rows)) + "\n")

    rows = []
    for n in sorted(agg.n.unique()):
        for f in ("full", "halving", "splitting"):
            rows.append({"n": f"2^{int(np.log2(n))}", "find": f,
                         "naive ÷ rank (time)": f"{val('random', f'naive+{f}', n) / val('random', f'rank+{f}', n):.2f}",
                         "naive ÷ rank (reads)": f"{val('random', f'naive+{f}', n, 'reads') / val('random', f'rank+{f}', n, 'reads'):.2f}"})
    L.append("**H6 (compression without balanced linking), random workload.**\n")
    L.append(md_table(pd.DataFrame(rows)) + "\n")

    if validity is not None:
        p = validity[validity.experiment == "sink"].pivot_table(index="n", columns=["variant", "mode"],
                                                                values="ns_per_op")
        rows = []
        for n in p.index:
            row = {"n": f"2^{int(np.log2(n))}"}
            for var in ("naive+none", "rank+none", "rank+full"):
                row[f"{var} sink"] = f"{p.loc[n, (var, 'sink')]:.2f}"
                row[f"{var} no sink"] = f"{p.loc[n, (var, 'nosink')]:.2f}"
            rows.append(row)
        L.append("**H7 (measurement validity: the sink).** chain workload, ns/op, with and without the volatile sink.\n")
        L.append(md_table(pd.DataFrame(rows)) + "\n")

        p = validity[validity.experiment == "replay"].pivot_table(index="n", columns=["variant", "mode"],
                                                                  values="ns_per_op")
        rows = []
        for n in p.index:
            row = {"n": f"2^{int(np.log2(n))}"}
            for var in ("rank+full", "rank+halving", "size+full", "rank+none", "naive+full"):
                f, r = p.loc[n, (var, "fresh")], p.loc[n, (var, "replayed")]
                row[f"{var} fresh"] = f"{f:.2f}"
                row[f"{var} replayed"] = f"{r:.2f}"
                row[f"{var} fresh÷replayed"] = f"{f / r:.2f}"
            rows.append(row)
        L.append("**H8 (measurement validity: replayed sequences; found during the first full run, not "
                 "pre-registered).** random workload, ns/op, a fresh sequence per timed rep vs one sequence "
                 "replayed every rep.\n")
        L.append(md_table(pd.DataFrame(rows)) + "\n")

        t = validity[validity.experiment == "throttle"]
        if len(t):
            p = t.pivot_table(index="variant", columns="mode", values="ns_per_op")
            order = list(t[t["mode"] == "disabled"].variant)        # harness order
            rows = [{"variant (in measurement order)": v, "throttling disabled": f"{p.loc[v, 'disabled']:.2f}",
                     "throttling allowed": f"{p.loc[v, 'allowed']:.2f}",
                     "allowed ÷ disabled": f"{p.loc[v, 'allowed'] / p.loc[v, 'disabled']:.2f}"} for v in order]
            L.append(f"**H9 (measurement validity: Windows power throttling; found during the second full run, "
                     f"not pre-registered).** random workload, n = 2^{int(np.log2(t.n.iloc[0]))}, ns/op. naive+none "
                     "is measured first and runs for several seconds.\n")
            L.append(md_table(pd.DataFrame(rows)) + "\n")

    # Full tables at two sizes.
    L.append("## All variants at n = 2^20 and at the largest n\n")
    L.append("ns/op is the median over seeds (min–max in brackets). reads and writes are per operation.\n")
    for w in sorted(agg.workload.unique()):
        for n in (2**20, nmax):
            s = agg[(agg.workload == w) & (agg.n == n)].sort_values("ns")
            if s.empty:
                continue
            t = pd.DataFrame({
                "variant": s.variant,
                "ns/op": [f"{a:.2f} [{b:.2f}–{c:.2f}]" for a, b, c in zip(s.ns, s.ns_lo, s.ns_hi)],
                "reads/op": s.reads.map("{:.3f}".format), "writes/op": s.writes.map("{:.3f}".format),
                "path/find": s.path.map("{:.3f}".format), "final max depth": s.max_depth.astype(int)})
            L.append(f"### {w}, n = 2^{int(np.log2(n))}\n")
            L.append(md_table(t) + "\n")
    text = "\n".join(L)
    (out / "summary.md").write_text(text, encoding="utf-8")
    return text


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("results", nargs="?", default="results")
    args = ap.parse_args()
    res = Path(args.results)
    figs = res / "figures"
    figs.mkdir(parents=True, exist_ok=True)

    df = pd.read_csv(res / "results.csv")
    machine = json.loads((res / "machine.json").read_text()) if (res / "machine.json").exists() else {}
    validity = pd.read_csv(res / "validity.csv") if (res / "validity.csv").exists() else None
    caches = machine.get("caches_bytes", {})
    agg = aggregate(df)

    for w in sorted(agg.workload.unique()):
        workload_figure(agg, w, caches, figs)
    if {"chain", "binomial", "random"} <= set(agg.workload):
        theory_figure(agg, figs)
    if "random" in set(agg.workload):
        time_vs_work_figure(agg, caches, figs)
    if {"local", "local-shuffled"} <= set(agg.workload):
        locality_figure(agg, caches, figs)
    if validity is not None:
        validity_figure(validity, figs)
    summary(df, agg, validity, machine, res)
    print(f"wrote {figs}/*.png and {res / 'summary.md'}")


if __name__ == "__main__":
    main()
