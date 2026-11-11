description: Market data patchbays: a price ticker, OHLC bars, and a multi-asset JSON firehose demuxed and cleaned at the broker.

# Market data

{lede} High-frequency market data is a poster case for monoblok: data moves fast, most of the movement is not worth a downstream message, and every subscriber would otherwise re-implement the same rounding, dedupe and demux logic.

## Price ticker

{tag:Intermediate} Two rules and an export. Prices arrive on `MARKET.<SYM>`; subscribers get a deduplicated mirror, and big jumps are fanned out to an alerts subject that is forwarded to a real NATS server.

{{include examples/ticker.edn}}

```soundcheck examples/ticker.edn
MARKET.AAPL|182.3401
MARKET.AAPL|182.3402
MARKET.AAPL|182.351
MARKET.AAPL|183.9
```

[`delta`](forms.html#idempotent-filters) returns the difference from the previous value on the same subject (0 on first sight), so the alert rule stays quiet until it has a prior price. [`(subject-token 1)`](forms.html#strings-and-subjects) pulls the symbol out of `MARKET.AAPL` to build a per-symbol alert subject. The [`export`](bridge.html#export) form forwards only `alerts.market.>`; the raw ticks and the stable mirror stay local.

## OHLC bars

{tag:Intermediate} [`bar!`](forms.html#bars) folds samples into open / high / low / close bars per symbol, closing either every N ticks or on wall-clock boundaries.

{{include examples/bars.edn}}

The bar rule is `:reentrant true` only because the second rule consumes the derived `MARKET.<SYM>.bar.close` stream. Alerting on closed bars instead of raw ticks keeps the alert free of intra-bar noise. A subscriber to `MARKET.AAPL.bar.>` sees a burst of four messages per closed bar, in open / high / low / close order. In-progress bars are part of the [snapshot](state.html#snapshots), so a restart mid-bar resumes without losing the open, high and low collected so far.

```sh
examples/bars.sh
```

## A market data firehose

{tag:Advanced} Imagine your data provider gives you a stream of JSON frames that carry several fields per message, and every downstream subscriber re-implements the same demux, round, dedupe and alert logic. At scale, doing this n times causes read/write amplification and much wasted work, not to mention subtle bugs. Doing it once at the broker means subscribers can use subject filtering to pick the slice they actually need and ignore the rest.

The [`json-massive`](https://github.com/lexvicacom/monoblok/tree/main/examples/json-massive) example simulates a Massive-shape websocket feed (stocks, options, crypto, forex). It has two pieces: `mock_producer.js`, a synthetic producer that speaks the NATS protocol over a plain socket and publishes frames on subjects shaped `<ev>.<symbol>`, and `massive.edn`, the patchbay below. There is more background in the [blog post](https://alexjreid.dev/posts/monoblok-massive/).

For stock trades the whole conditioning layer is three rules, each feeding the next:

1. `(json-demux! ...)` on `T.*` splits the frame into one subject per field: `T.AAPL.p` (price) and `T.AAPL.s` (size).
2. A rule on the demuxed `T.*.p` rounds to 2dp, drops duplicates and mirrors to `T.<SYM>.p.stable`.
3. A second rule on `T.*.p` alerts on a jump of 1.00 or more between consecutive ticks on the same symbol.

Rule 2 matches `T.*.p`, a subject that only exists because rule 1 emitted it. That works because rule 1 is marked [`:reentrant true`](forms.html#re-entry). Options come along free (the `T.*` wildcard matches `T.O:AAPL250620C00200000` too), and crypto and FX repeat the pattern with their own thresholds.

{{include examples/json-massive/massive.edn}}

Five frames through the trade and crypto rules:

```soundcheck examples/json-massive/massive.edn
T.AAPL|{"ev":"T","sym":"AAPL","p":187.421,"s":100}
T.AAPL|{"ev":"T","sym":"AAPL","p":187.424,"s":50}
T.AAPL|{"ev":"T","sym":"AAPL","p":188.95,"s":200}
XT.BTC-USD|{"p":64000,"s":0.1}
XT.BTC-USD|{"p":64700,"s":0.2}
```

The second AAPL trade rounds to the same 187.42, so the stable mirror stays quiet. The third moves 1.526 and raises `alerts.trade.AAPL`. The second BTC trade is a 1.08% move and raises `alerts.crypto.BTC-USD`.

A subscriber that only cares about price changes subscribes to `T.*.p.stable` and gets a clean, per-symbol stream of rounded floats. No JSON parsing, and no need for every consumer to chew through the firehose and repeat the same logic.

### Run it

```sh
# terminal 1: start monoblok with the demuxing patchbay
# if you don't need the last-value cache, save some cpu by turning it off
monoblok --no-lvc examples/json-massive/massive.edn

# terminal 2: start the producer (RATE=2 doubles the message rate)
node examples/json-massive/mock_producer.js

# terminal 3: peek at everything
nats sub '>'
```

Or let the script do all three for five seconds and print tables of the raw frames, the demuxed prices, the stable mirror and the alerts side by side:

```sh
./examples/json-massive/run.sh
```

### Last prints for slow markets and the close

A dashboard opening on Sunday afternoon wants to render the last print for every symbol it tracks. Nothing is publishing, the market is closed, and the patchbay has been idle for two days. Opt the stable mirror into the last-value cache:

```edn
(lvc ["T.*.p.stable"])
```

Subscribe to `$LVC.T.*.p.stable` and the broker hands back the most recent rounded price for every symbol it has seen, then nothing else until trading resumes. The same subscribe works for a logger restarting mid-session, or a tool that wants the last print on a thinly-traded name where ticks arrive minutes apart. The dedupe rule is what makes the `.stable` cache useful: without `squelch`, the cached value would be whatever noisy reprint happened to land last, not the last meaningful price.

### Already-aggregated channels

The `AM` / `XA` / `CA` channels deliver server-side OHLC bars on delayed feeds, so the patchbay demuxes those frames into scalar `o` / `h` / `l` / `c` / `v` streams instead of recomputing bars from raw trades. If you only have a trade stream and need to build bars locally, use [`bar!`](#ohlc-bars).

### Bridging to NATS

Subscribers can hook up directly to monoblok. Most of the time, though, you already have a NATS cluster doing the heavy lifting for the rest of the system, and you want the conditioned market data to land there too. Add an [`export`](bridge.html#export) form pointing at `T.*.p.stable` and JetStream on the far side stores the deduplicated, rounded prices, not the raw firehose:

```edn
(export
  :servers ["nats://nats.internal:4222"]
  :name    "monoblok-market"
  :export  ["T.*.p.stable" "alerts.>"])
```

Less write amplification, less retained noise, and only the precision a consumer needs. Replays are smaller and faster, and consumers reading from the stream get the same shape of data as live subscribers on the broker, so a backfill and a live tail stitch together cleanly. The raw frames stay on the monoblok side for anyone who wants them.

## Budgeting bar state

{tag:Advanced} `bar!` is cheap per subject, but it is stateful. A local bar builder keeps one in-progress bar slot for every distinct market/symbol subject the rule has seen. Thousands of symbols across tens of markets can therefore turn into tens or hundreds of thousands of long-lived slots.

{{include examples/json-massive/bar-cardinality.edn}}

The harness drives that patchbay through `--soundcheck` and reports max RSS:

```sh
# defaults: MARKETS=20 SYMBOLS=5000 TICKS=1 BAR_WINDOW=100000000
examples/json-massive/measure-bar-cardinality.sh

# scale the subject universe and keep bars open longer
MARKETS=50 SYMBOLS=10000 TICKS=1 BAR_WINDOW=1000000000 examples/json-massive/measure-bar-cardinality.sh
```

The important dimension is `MARKETS * SYMBOLS`: increasing `TICKS` mostly reuses existing slots, while increasing the subject universe creates more retained patchbay state. The same reasoning applies to every stateful form: narrow the filter when you can, and measure the shape you plan to run.
