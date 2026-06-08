# data/

## Normalized message CSV format

The engine's external-data seam is a single normalized schema (one message per
row, header required):

```
ts,type,id,side,price,qty,new_price,new_qty
```

| column        | meaning                                                        |
|---------------|----------------------------------------------------------------|
| `ts`          | timestamp / sequence number (uint)                             |
| `type`        | `L`=Limit, `M`=Market, `C`=Cancel, `X`=Modify                  |
| `id`          | order id (uint, unique among live orders)                      |
| `side`        | `B`=Buy, `S`=Sell (ignored for Cancel)                         |
| `price`       | price **in ticks** (ignored for Market/Cancel)                 |
| `qty`         | quantity (ignored for Cancel)                                  |
| `new_price`   | Modify only: replacement price in ticks                        |
| `new_qty`     | Modify only: replacement quantity                              |

Unused fields are written as `0`. `Modify` is cancel-replace (loses time
priority). See `include/lob/replay.hpp`.

`sample_messages.csv` is a tiny hand-written scenario (resting orders, a
crossing market order, a cancel and a modify) used for documentation and quick
manual replay:

```bash
# replay through the optimised engine from Python
export PYTHONPATH=$PWD/build:$PYTHONPATH
python -c "import lobpy; s=lobpy.replay_csv('data/sample_messages.csv', num_ticks=200000); \
print('trades:', len(s['trades']['price']))"
```

## Synthetic data

The primary data source is the seeded C++ generator
(`include/lob/generator.hpp`), reproducible from a seed and able to emit millions
of messages. To dump a synthetic stream to CSV use `write_csv` from
`include/lob/replay.hpp`, or simply call `lobpy.simulate(...)` which generates
and replays in one step.

## LOBSTER drop-in (optional, external)

[LOBSTER](https://lobsterdata.com) publishes free sample message + orderbook
files for instruments like AAPL/MSFT. LOBSTER *message* files have columns:

```
Time, Type, OrderID, Size, Price, Direction
# Type: 1=new limit, 2=partial cancel, 3=delete, 4=exec visible, 5=exec hidden, 7=halt
# Direction: 1=buy, -1=sell   Price in (10^-4) dollars
```

To replay LOBSTER data here, convert it to the normalized schema above:

1. Map `Price` (10^-4 USD) to integer ticks (e.g. divide by the instrument tick
   size, commonly 100 → cents).
2. Map `Type`: `1`→`L`; `3`→`C`; `2`→`X` (modify down to remaining size); the
   `4/5` execution rows are the resulting tape (used to validate, not replayed,
   since matching is reproduced by the engine).
3. Map `Direction`: `1`→`B`, `-1`→`S`.
4. Choose `num_ticks` to cover the file's price range.

Drop converted files under `data/lobster/` (git-ignored). With both a LOBSTER
message file and its orderbook snapshot file, the reconstructed top-of-book from
this engine can be diffed against LOBSTER's published book to validate against
real exchange data. No LOBSTER files are bundled (licensing); the synthetic
generator is the default and the golden Ref==Fast test is the primary
correctness guarantee.
