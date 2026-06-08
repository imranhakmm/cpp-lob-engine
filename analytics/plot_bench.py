#!/usr/bin/env python3
"""Render the latency/throughput figure from docs/bench_results.csv.

Run the C++ harness first (from the repo root) so the CSV exists:
    ./build/lob_bench_harness 2000000 7
    python analytics/plot_bench.py
"""
from __future__ import annotations

import pathlib

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

ROOT = pathlib.Path(__file__).resolve().parent.parent
CSV = ROOT / "docs" / "bench_results.csv"
OUT = ROOT / "docs" / "bench_perf.png"


def main() -> None:
    if not CSV.exists():
        raise SystemExit(f"{CSV} not found -- run ./build/lob_bench_harness first")
    df = pd.read_csv(CSV).set_index("engine")

    fig, (ax_t, ax_l) = plt.subplots(1, 2, figsize=(12, 5))
    colors = {"OrderBookRef": "#888888", "OrderBookFast": "#1f77b4"}
    order = [e for e in ["OrderBookRef", "OrderBookFast"] if e in df.index]

    # Throughput bar chart.
    tput = [df.loc[e, "throughput_mmsg_s"] for e in order]
    bars = ax_t.bar(order, tput, color=[colors[e] for e in order])
    ax_t.set_ylabel("Throughput (million msgs / sec)")
    ax_t.set_title("Sustained throughput (higher is better)")
    for b, v in zip(bars, tput):
        ax_t.text(b.get_x() + b.get_width() / 2, v, f"{v:.1f}",
                  ha="center", va="bottom")

    # Latency percentile comparison (log scale).
    pcts = ["p50_ns", "p90_ns", "p99_ns", "p999_ns"]
    labels = ["p50", "p90", "p99", "p99.9"]
    x = range(len(pcts))
    width = 0.38
    for i, e in enumerate(order):
        vals = [df.loc[e, p] for p in pcts]
        ax_l.bar([xi + (i - 0.5) * width for xi in x], vals, width,
                 label=e, color=colors[e])
    ax_l.set_xticks(list(x))
    ax_l.set_xticklabels(labels)
    ax_l.set_yscale("log")
    ax_l.set_ylabel("Per-message latency (ns, log scale)")
    ax_l.set_title("Latency percentiles (lower is better)")
    ax_l.legend()

    fig.suptitle("cpp-lob-engine: OrderBookRef vs OrderBookFast", fontweight="bold")
    fig.tight_layout()
    fig.savefig(OUT, dpi=120)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
