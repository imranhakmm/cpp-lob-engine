"""Microstructure features from a reconstructed book + trade tape.

Input is the columnar output of ``lobpy.simulate`` / ``lobpy.replay_csv``:

    sim["l1"]     -> ts, best_bid, best_ask, bid_size, ask_size  (per message)
    sim["trades"] -> ts, price, qty, taker_side, taker_id, maker_id

All prices are in ticks. Features implemented:

  * mid-price and micro-price (size-weighted)
  * order-flow imbalance (OFI) and queue/volume imbalance
  * Lee-Ready trade-sign classification (tick-test fallback)
  * realized spread at a horizon
  * multi-horizon mark-outs

The functions are deliberately vectorised numpy/pandas so the notebook stays
fast on hundreds of thousands of events.
"""
from __future__ import annotations

import numpy as np
import pandas as pd

BUY, SELL = 0, 1


def l1_frame(sim: dict) -> pd.DataFrame:
    """Build a per-message L1 DataFrame with mid / micro-price.

    Rows where either side is empty (best == -1) are dropped: spreads and
    weighted prices are undefined with a one-sided book.
    """
    l1 = sim["l1"]
    df = pd.DataFrame(
        {
            "ts": l1["ts"],
            "best_bid": l1["best_bid"].astype(np.float64),
            "best_ask": l1["best_ask"].astype(np.float64),
            "bid_size": l1["bid_size"].astype(np.float64),
            "ask_size": l1["ask_size"].astype(np.float64),
        }
    )
    df = df[(df.best_bid >= 0) & (df.best_ask >= 0)].reset_index(drop=True)

    df["mid"] = 0.5 * (df.best_bid + df.best_ask)
    df["spread"] = df.best_ask - df.best_bid
    # Micro-price: weight each side by the *opposite* queue size, so price leans
    # toward the side more likely to be hit.
    denom = (df.bid_size + df.ask_size).replace(0, np.nan)
    df["micro_price"] = (df.best_bid * df.ask_size + df.best_ask * df.bid_size) / denom
    df["queue_imbalance"] = (df.bid_size - df.ask_size) / denom
    return df


def order_flow_imbalance(df: pd.DataFrame) -> pd.Series:
    """Cont-Kukanov-Stoikov order-flow imbalance from consecutive L1 states.

    OFI_t = dBidSize*(bid up/flat) - dAskSize*(ask down/flat), with full
    add/remove on a price move. Positive => net buy pressure.
    """
    bb, ba = df.best_bid.to_numpy(), df.best_ask.to_numpy()
    bs, as_ = df.bid_size.to_numpy(), df.ask_size.to_numpy()
    ofi = np.zeros(len(df))
    for i in range(1, len(df)):
        if bb[i] > bb[i - 1]:
            db = bs[i]
        elif bb[i] == bb[i - 1]:
            db = bs[i] - bs[i - 1]
        else:
            db = -bs[i - 1]
        if ba[i] < ba[i - 1]:
            da = as_[i]
        elif ba[i] == ba[i - 1]:
            da = as_[i] - as_[i - 1]
        else:
            da = -as_[i - 1]
        ofi[i] = db - da
    return pd.Series(ofi, index=df.index, name="ofi")


def trades_frame(sim: dict, l1: pd.DataFrame) -> pd.DataFrame:
    """Trade tape enriched with Lee-Ready sign and the prevailing mid."""
    tr = sim["trades"]
    t = pd.DataFrame(
        {
            "ts": tr["ts"],
            "price": tr["price"].astype(np.float64),
            "qty": tr["qty"].astype(np.float64),
            "taker_side": tr["taker_side"].astype(np.int8),
        }
    )
    if t.empty:
        t["mid"] = []
        t["sign"] = []
        return t

    # Prevailing mid: most recent L1 mid at-or-before each trade ts.
    mid_lookup = l1[["ts", "mid"]].drop_duplicates("ts", keep="last")
    t = pd.merge_asof(
        t.sort_values("ts"), mid_lookup.sort_values("ts"), on="ts", direction="backward"
    )

    # Lee-Ready: quote test vs the prevailing mid; tick-test fallback on ties.
    sign = np.where(t.price > t.mid, 1, np.where(t.price < t.mid, -1, 0))
    last = 0
    out = np.empty(len(sign), dtype=np.int64)
    prev_price = None
    for i in range(len(sign)):
        s = sign[i]
        if s == 0:  # at the mid -> tick test
            if prev_price is None or t.price.iat[i] == prev_price:
                s = last
            else:
                s = 1 if t.price.iat[i] > prev_price else -1
        out[i] = s
        last = s if s != 0 else last
        prev_price = t.price.iat[i]
    t["sign"] = out
    return t


def realized_spread(trades: pd.DataFrame, l1: pd.DataFrame, horizon: int) -> pd.Series:
    """Realized spread: 2 * sign * (trade_price - mid_{t+h}), in ticks.

    Measures the portion of the effective spread retained after the market moves
    `horizon` messages forward (lower => more adverse selection).
    """
    future = l1[["ts", "mid"]].copy()
    future["ts_key"] = future["ts"] - horizon  # mid `horizon` steps ahead
    merged = pd.merge_asof(
        trades.sort_values("ts"),
        future.sort_values("ts_key")[["ts_key", "mid"]].rename(
            columns={"ts_key": "ts", "mid": "mid_future"}
        ),
        on="ts",
        direction="forward",
    )
    rs = 2.0 * merged["sign"] * (merged["price"] - merged["mid_future"])
    return pd.Series(rs.to_numpy(), name=f"realized_spread_h{horizon}")


def markouts(trades: pd.DataFrame, l1: pd.DataFrame, horizons) -> pd.DataFrame:
    """Signed mark-out (mid_{t+h} - mid_t) * sign for several horizons (ticks)."""
    mid_lookup = l1[["ts", "mid"]].drop_duplicates("ts", keep="last").sort_values("ts")
    out = {}
    for h in horizons:
        fut = mid_lookup.copy()
        fut["ts"] = fut["ts"] - h
        m = pd.merge_asof(
            trades.sort_values("ts"),
            fut.rename(columns={"mid": "mid_future"}),
            on="ts",
            direction="forward",
        )
        out[f"h{h}"] = (m["sign"] * (m["mid_future"] - m["mid"])).to_numpy()
    return pd.DataFrame(out, index=trades.index)


def build_features(sim: dict, markout_horizons=(1, 5, 20, 100)) -> dict:
    """One-call pipeline returning the L1, trade and mark-out frames."""
    l1 = l1_frame(sim)
    l1["ofi"] = order_flow_imbalance(l1)
    # short-horizon forward mid return (in ticks) for OFI/return studies
    l1["fwd_ret_10"] = l1["mid"].shift(-10) - l1["mid"]
    trades = trades_frame(sim, l1)
    mo = markouts(trades, l1, markout_horizons) if not trades.empty else pd.DataFrame()
    if not trades.empty:
        trades["realized_spread_h20"] = realized_spread(trades, l1, 20).to_numpy()
    return {"l1": l1, "trades": trades, "markouts": mo}
