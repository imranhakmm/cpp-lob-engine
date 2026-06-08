#!/usr/bin/env python3
"""End-to-end analytics run: simulate -> features -> parquet/CSV + plots.

    export PYTHONPATH=$PWD/build:$PYTHONPATH
    python analytics/run_analytics.py [n] [seed]

Writes feature tables and figures under analytics/out/.
"""
from __future__ import annotations

import pathlib
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

import features as feat

try:
    import lobpy
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "lobpy not importable -- build it and add build/ to PYTHONPATH:\n"
        "  cmake --build build -j && export PYTHONPATH=$PWD/build:$PYTHONPATH"
    ) from exc

ROOT = pathlib.Path(__file__).resolve().parent
OUT = ROOT / "out"


def save_table(df, name: str) -> None:
    try:
        df.to_parquet(OUT / f"{name}.parquet")
    except Exception:  # parquet engine missing -> CSV fallback
        df.to_csv(OUT / f"{name}.csv", index=False)


def main() -> None:
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200_000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 7
    OUT.mkdir(exist_ok=True)

    print(f"simulating {n} messages (seed={seed}) ...")
    sim = lobpy.simulate(n=n, seed=seed)
    f = feat.build_features(sim)
    l1, trades, mo = f["l1"], f["trades"], f["markouts"]
    print(f"L1 rows={len(l1)}  trades={len(trades)}")

    save_table(l1, "l1_features")
    save_table(trades, "trades_features")
    if not mo.empty:
        save_table(mo, "markouts")

    # --- Figure 1: mid / micro-price and spread over time -------------------
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 7), sharex=True)
    win = slice(0, min(5000, len(l1)))
    ax1.plot(l1.mid.values[win], label="mid", lw=0.8)
    ax1.plot(l1.micro_price.values[win], label="micro-price", lw=0.8, alpha=0.8)
    ax1.set_ylabel("price (ticks)")
    ax1.set_title("Mid vs micro-price")
    ax1.legend()
    ax2.plot(l1.spread.values[win], lw=0.6, color="#d62728")
    ax2.set_ylabel("spread (ticks)")
    ax2.set_xlabel("message index")
    ax2.set_title("Quoted spread")
    fig.tight_layout()
    fig.savefig(OUT / "midprice_spread.png", dpi=110)
    plt.close(fig)

    # --- Figure 2: OFI vs short-horizon forward return ----------------------
    sub = l1.dropna(subset=["ofi", "fwd_ret_10"])
    fig, ax = plt.subplots(figsize=(7, 6))
    ax.scatter(sub.ofi, sub.fwd_ret_10, s=3, alpha=0.15, color="#1f77b4")
    if len(sub) > 2 and sub.ofi.std() > 0:
        b, a = np.polyfit(sub.ofi, sub.fwd_ret_10, 1)
        xs = np.linspace(sub.ofi.min(), sub.ofi.max(), 50)
        corr = np.corrcoef(sub.ofi, sub.fwd_ret_10)[0, 1]
        ax.plot(xs, a + b * xs, color="#d62728",
                label=f"fit slope={b:.3e}\ncorr={corr:.3f}")
        ax.legend()
    ax.set_xlabel("order-flow imbalance (OFI)")
    ax.set_ylabel("forward 10-msg mid return (ticks)")
    ax.set_title("OFI predicts short-horizon return")
    fig.tight_layout()
    fig.savefig(OUT / "ofi_vs_return.png", dpi=110)
    plt.close(fig)

    # --- Figure 3: average mark-out curve by trade sign ---------------------
    if not mo.empty:
        horizons = [int(c[1:]) for c in mo.columns]
        avg = [mo[c].mean() for c in mo.columns]
        fig, ax = plt.subplots(figsize=(7, 5))
        ax.plot(horizons, avg, marker="o", color="#2ca02c")
        ax.axhline(0, color="grey", lw=0.7)
        ax.set_xlabel("horizon (messages)")
        ax.set_ylabel("avg signed mark-out (ticks)")
        ax.set_title("Mark-out curve (post-trade price drift, sign-adjusted)")
        fig.tight_layout()
        fig.savefig(OUT / "markout_curve.png", dpi=110)
        plt.close(fig)

    # --- Console summary ----------------------------------------------------
    print("\n=== summary ===")
    print(f"mean spread          : {l1.spread.mean():.3f} ticks")
    print(f"mean |queue imbalance|: {l1.queue_imbalance.abs().mean():.3f}")
    if not trades.empty:
        buys = int((trades.sign > 0).sum())
        sells = int((trades.sign < 0).sum())
        print(f"trades buy/sell      : {buys}/{sells}")
        print(f"mean realized spread : {trades.realized_spread_h20.mean():.3f} ticks (h=20)")
    if not mo.empty:
        print("mark-outs (ticks)    : " +
              ", ".join(f"{c}={mo[c].mean():.3f}" for c in mo.columns))
    print(f"\nwrote tables + figures to {OUT}")


if __name__ == "__main__":
    main()
