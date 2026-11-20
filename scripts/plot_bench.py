#!/usr/bin/env python3
import argparse
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def plot(rows, metric, ylabel, path):
    fig, axes = plt.subplots(2, 2, figsize=(10, 6), layout="constrained")
    for ax, workload in zip(axes.flat, ("fillseq", "fillrandom", "readrandom", "mixed")):
        for mode, color in (("always", "#2053a0"), ("batch", "#b44829")):
            cases = sorted(
                (int(row["threads"]), float(row[metric]))
                for row in rows
                if row["workload"] == workload and row["sync_mode"] == mode
            )
            ax.plot(*zip(*cases), "o-", label=mode, color=color)
        ax.set(title=workload, xlabel="Threads", ylabel=ylabel, xticks=(1, 2, 4, 8))
        ax.grid(alpha=0.2)
        ax.legend()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path, nargs="?", default=Path("results/bench.csv"))
    args = parser.parse_args()
    with args.csv.open(newline="") as source:
        rows = list(csv.DictReader(source))
    plot(rows, "ops_per_sec", "Operations / second", args.csv.parent / "throughput.png")
    plot(rows, "p99_us", "p99 latency (µs)", args.csv.parent / "latency.png")


if __name__ == "__main__":
    main()
