"""plot.py — turn results.csv into one figure per workload.

Usage:
    python3 scripts/plot.py results/results.csv [--out results/]

Each figure plots ns per operation against n (log2 x-axis), one line per
variant. A flat line means constant amortised cost per operation.
Requires pandas and matplotlib.
"""

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd

LINK_COLOURS = {"naive": "tab:red", "rank": "tab:blue", "size": "tab:green"}
FIND_STYLES = {"none": ":", "full": "-", "halving": "--", "splitting": "-."}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv")
    parser.add_argument("--out", default="results")
    args = parser.parse_args()

    df = pd.read_csv(args.csv)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    for workload, group in df.groupby("workload"):
        fig, ax = plt.subplots(figsize=(8, 6))
        for (link, find), g in group.groupby(["link", "find"]):
            g = g.sort_values("n")
            ax.plot(
                g["n"], g["ns_per_op"],
                color=LINK_COLOURS.get(link, "black"),
                linestyle=FIND_STYLES.get(find, "-"),
                marker="o", markersize=3,
                label=f"{link} + {find}",
            )
        ax.set_xscale("log", base=2)
        # The naive chain line dwarfs everything else; log y keeps the rest visible.
        if workload == "chain":
            ax.set_yscale("log")
        ax.set_xlabel("n (elements)")
        ax.set_ylabel("time per operation (ns)")
        ax.set_title(f"Union-Find variants — {workload} workload")
        ax.grid(True, which="both", alpha=0.3)
        # Legend below the axes so it never covers data points.
        ax.legend(fontsize=8, ncol=4, title="link + find",
                  loc="upper center", bbox_to_anchor=(0.5, -0.13))
        fig.tight_layout()
        path = out / f"{workload}.png"
        fig.savefig(path, dpi=150)
        plt.close(fig)
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
