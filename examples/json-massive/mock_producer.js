#!/usr/bin/env node
// Synthetic NATS producer simulating a Massive market-data feed (the
// rebranded Polygon-shape WebSocket events). Connects to a locally running
// monoblok / NATS server on 127.0.0.1:4222 and publishes JSON event frames
// on subjects shaped <ev>.<symbol-or-pair>:
//
//   T.AAPL                     stock trade
//   Q.AAPL                     stock NBBO quote
//   AM.AAPL                    stock minute aggregate
//   T.O:AAPL250620C00150000    option trade   (OCC symbol prefixed O:)
//   Q.O:AAPL250620C00150000    option quote
//   XT.BTC-USD                 crypto trade
//   XQ.BTC-USD                 crypto quote
//   XA.BTC-USD                 crypto minute aggregate
//   C.EURUSD                   forex quote
//   CA.EURUSD                  forex minute aggregate
//
// JSON field shapes match the docs (massive.com/docs/{stocks,options,
// crypto,forex}). Crypto/forex use a `pair` field instead of `sym`, and
// forex quotes use single-letter `b` / `a` price fields with no sizes.
//
// Asset classes tick at different baseline rates; the RATE env var scales
// every interval (RATE=2 → twice as fast, RATE=0.5 → half speed).

const net = require('net');

const HOST = process.env.NATS_HOST || '127.0.0.1';
const PORT = parseInt(process.env.NATS_PORT || '4222', 10);
const RATE = Math.max(0.01, parseFloat(process.env.RATE || '1'));

function log(...args) {
    const ts = new Date().toISOString().slice(11, 23);
    console.log(`[${ts}]`, ...args);
}

// ---------------------------------------------------------------------------
// Symbol universe
// ---------------------------------------------------------------------------

const EQUITIES = [
    'AAPL', 'MSFT', 'GOOG', 'AMZN', 'META', 'NVDA', 'TSLA', 'AMD',
    'NFLX', 'INTC', 'CSCO', 'ORCL', 'IBM', 'CRM', 'ADBE', 'QCOM',
    'JPM', 'BAC', 'GS', 'WFC', 'V', 'MA',
    'XOM', 'CVX', 'KO', 'PEP', 'WMT', 'COST', 'DIS', 'NKE',
    'SPY', 'QQQ', 'IWM', 'DIA',
];

// OCC option symbol: O:<root><YYMMDD><C|P><strike*1000, 8-digit zero-padded>
function buildOptions() {
    const out = [];
    const exp = '250620';
    const underlyings = [
        { sym: 'AAPL', atm: 200 },
        { sym: 'MSFT', atm: 420 },
        { sym: 'NVDA', atm: 130 },
        { sym: 'SPY',  atm: 540 },
        { sym: 'TSLA', atm: 250 },
    ];
    for (const u of underlyings) {
        for (let k = -2; k <= 2; k++) {
            const strike = u.atm + k * 5;
            const strikeStr = String(strike * 1000).padStart(8, '0');
            for (const cp of ['C', 'P']) {
                out.push(`O:${u.sym}${exp}${cp}${strikeStr}`);
            }
        }
    }
    return out;
}
const OPTIONS = buildOptions();

// Crypto pairs are bare {from}-{to} on the wire (no X: prefix).
const CRYPTO = [
    'BTC-USD', 'ETH-USD', 'SOL-USD', 'XRP-USD',
    'DOGE-USD', 'ADA-USD', 'AVAX-USD', 'LINK-USD',
    'MATIC-USD', 'LTC-USD',
];

// Forex pairs: 6-char concat (e.g. EURUSD).
const FOREX = [
    'EURUSD', 'GBPUSD', 'USDJPY', 'USDCHF',
    'AUDUSD', 'USDCAD', 'NZDUSD', 'EURGBP', 'EURJPY',
];

// ---------------------------------------------------------------------------
// Synthetic price model: seeded per symbol, light random walk so values
// drift instead of teleporting around their base.
// ---------------------------------------------------------------------------

function seedFor(sym) {
    let h = 0;
    for (const ch of sym) h = (h * 31 + ch.charCodeAt(0)) & 0xffffffff;
    return h >>> 0;
}

function basePrice(sym, kind) {
    const h = seedFor(sym);
    switch (kind) {
        case 'equity':  return 50 + (h % 950);              // 50..1000
        case 'option':  return 0.5 + ((h % 5000) / 100);    // 0.50..50.50
        case 'crypto':
            if (sym.startsWith('BTC')) return 65000 + (h % 10000);
            if (sym.startsWith('ETH')) return 3000 + (h % 800);
            if (sym.startsWith('SOL')) return 140 + (h % 50);
            return 0.5 + ((h % 200000) / 1000);
        case 'forex':
            if (sym.includes('JPY')) return 100 + (h % 60);
            return 0.5 + ((h % 1500) / 1000);
    }
}

const lastPrice = {};
function nextPrice(sym, kind) {
    let p = lastPrice[sym];
    if (p == null) p = basePrice(sym, kind);
    const vol = kind === 'crypto' ? 0.005
              : kind === 'option' ? 0.01
              : kind === 'forex'  ? 0.0005
              : 0.002;
    const drift = (Math.random() - 0.5) * 2 * vol * p;
    p = Math.max(0.01, p + drift);
    lastPrice[sym] = p;
    return p;
}

const round4 = (x) => +x.toFixed(4);

// ---------------------------------------------------------------------------
// Event builders: match docs at massive.com/docs/{stocks,options,crypto,forex}.
// ---------------------------------------------------------------------------

// Stock + option trade share the T shape (options omit z/i/q-detail but
// extra fields are harmless on the wire; we keep the union to stay simple).
function stockTrade(sym) {
    const p = nextPrice(sym, 'equity');
    const t = Date.now();
    return {
        ev: 'T', sym,
        x: 4, i: String(Math.floor(Math.random() * 1e9)), z: 3,
        p: round4(p),
        s: Math.floor(Math.random() * 1000) + 1,
        c: [0],
        t,
        q: Math.floor(Math.random() * 1e9),
    };
}

function stockQuote(sym) {
    const p = nextPrice(sym, 'equity');
    return {
        ev: 'Q', sym,
        bx: 11, bp: round4(p - 0.005), bs: Math.floor(Math.random() * 500) + 1,
        ax: 12, ap: round4(p + 0.005), as: Math.floor(Math.random() * 500) + 1,
        c: 0, z: 3,
        t: Date.now(),
        q: Math.floor(Math.random() * 1e9),
    };
}

function stockMinuteAgg(sym) {
    const p = nextPrice(sym, 'equity');
    const now = Date.now();
    const o = round4(p * (1 + (Math.random() - 0.5) * 0.005));
    const h = round4(Math.max(o, p) * (1 + Math.random() * 0.003));
    const l = round4(Math.min(o, p) * (1 - Math.random() * 0.003));
    return {
        ev: 'AM', sym,
        v: Math.floor(Math.random() * 100000),
        av: Math.floor(Math.random() * 1000000),
        op: round4(p - 0.5),
        vw: round4(p - 0.05),
        o, c: round4(p), h, l,
        a: round4(p - 0.05),
        z: Math.floor(Math.random() * 200) + 50,
        s: now - 60000, e: now,
    };
}

function optionTrade(sym) {
    const p = nextPrice(sym, 'option');
    return {
        ev: 'T', sym,
        x: 300,
        p: round4(p),
        s: Math.floor(Math.random() * 50) + 1,
        c: [],
        t: Date.now(),
        q: Math.floor(Math.random() * 1e9),
    };
}

function optionQuote(sym) {
    const p = nextPrice(sym, 'option');
    const spread = Math.max(0.05, p * 0.02);
    return {
        ev: 'Q', sym,
        bx: 300, bp: round4(p - spread / 2), bs: Math.floor(Math.random() * 100) + 1,
        ax: 313, ap: round4(p + spread / 2), as: Math.floor(Math.random() * 100) + 1,
        t: Date.now(),
        q: Math.floor(Math.random() * 1e9),
    };
}

function cryptoTrade(pair) {
    const p = nextPrice(pair, 'crypto');
    return {
        ev: 'XT', pair,
        p: round4(p),
        s: +(Math.random() * 5).toFixed(6),
        c: [Math.random() < 0.5 ? 1 : 2],
        i: String(Math.floor(Math.random() * 1e9)),
        x: 1,
        t: Date.now(),
        r: Date.now(),
    };
}

function cryptoQuote(pair) {
    const p = nextPrice(pair, 'crypto');
    const spread = p * 0.0005;
    return {
        ev: 'XQ', pair,
        bp: round4(p - spread / 2),
        bs: +(Math.random() * 3).toFixed(6),
        ap: round4(p + spread / 2),
        as: +(Math.random() * 3).toFixed(6),
        x: 1,
        t: Date.now(),
        r: Date.now(),
    };
}

function cryptoMinuteAgg(pair) {
    const p = nextPrice(pair, 'crypto');
    const now = Date.now();
    const o = round4(p * (1 + (Math.random() - 0.5) * 0.01));
    const h = round4(Math.max(o, p) * (1 + Math.random() * 0.005));
    const l = round4(Math.min(o, p) * (1 - Math.random() * 0.005));
    return {
        ev: 'XA', pair,
        o, c: round4(p), h, l,
        v: +(Math.random() * 1000).toFixed(2),
        vw: round4(p - 0.5),
        z: Math.floor(Math.random() * 50) + 5,
        s: now - 60000, e: now,
    };
}

// Forex quote: per docs, `b`/`a` are bid/ask (no sizes), and the pair is
// carried in `p` (yes, the docs really do collide with stocks' price field).
// We emit `pair` as a sibling so subscribers don't have to reverse-engineer
// the symbol from the subject (keeps it friendly for downstream rules
// without breaking the documented shape).
function forexQuote(pair) {
    const mid = nextPrice(pair, 'forex');
    const spread = mid * 0.00005;
    return {
        ev: 'C', pair,
        p: pair,
        x: 48,
        b: round4(mid - spread / 2),
        a: round4(mid + spread / 2),
        t: Date.now(),
    };
}

function forexMinuteAgg(pair) {
    const p = nextPrice(pair, 'forex');
    const now = Date.now();
    const o = round4(p * (1 + (Math.random() - 0.5) * 0.001));
    const h = round4(Math.max(o, p) * (1 + Math.random() * 0.0005));
    const l = round4(Math.min(o, p) * (1 - Math.random() * 0.0005));
    return {
        ev: 'CA', pair,
        o, c: round4(p), h, l,
        v: Math.floor(Math.random() * 5000),
        s: now - 60000, e: now,
    };
}

// ---------------------------------------------------------------------------
// Minimal NATS client: just enough to CONNECT + PUB. We never SUB, so we
// only need to handle INFO (reply with CONNECT) and PING (reply PONG).
// ---------------------------------------------------------------------------

const sock = new net.Socket();
let connected = false;
let timers = [];
let rxBuf = '';

function publish(subject, payload) {
    if (!connected) return;
    const body = Buffer.from(payload);
    sock.write(`PUB ${subject} ${body.length}\r\n`);
    sock.write(body);
    sock.write('\r\n');
}

function emit(evt) {
    const sym = evt.sym || evt.pair;
    publish(`${evt.ev}.${sym}`, JSON.stringify(evt));
}

sock.on('connect', () => log(`connected to nats://${HOST}:${PORT}`));

sock.on('data', (chunk) => {
    rxBuf += chunk.toString('utf8');
    let nl;
    while ((nl = rxBuf.indexOf('\r\n')) !== -1) {
        const line = rxBuf.slice(0, nl);
        rxBuf = rxBuf.slice(nl + 2);
        if (line.startsWith('INFO ')) {
            sock.write('CONNECT {"verbose":false,"pedantic":false,"name":"mock_producer"}\r\n');
            connected = true;
            startScenarios();
        } else if (line === 'PING') {
            sock.write('PONG\r\n');
        } else if (line.startsWith('-ERR')) {
            log(line);
        }
    }
});

sock.on('error', (err) => log(`socket error: ${err.message}`));

sock.on('close', () => {
    log('disconnected');
    for (const t of timers) clearInterval(t);
    timers = [];
    connected = false;
    setTimeout(reconnect, 1000);
});

function reconnect() {
    log(`reconnecting to ${HOST}:${PORT}`);
    sock.connect(PORT, HOST);
}

// ---------------------------------------------------------------------------
// Scenarios: each one is an independent timer publishing to its own
// subject tree at its own cadence. Pick a random subset of symbols on
// each tick so the feed feels alive without bursting all at once.
// RATE scales every interval (RATE=2 halves the period).
// ---------------------------------------------------------------------------

function pickN(arr, n) {
    if (n >= arr.length) return arr.slice();
    const out = [];
    const seen = new Set();
    while (out.length < n) {
        const i = Math.floor(Math.random() * arr.length);
        if (seen.has(i)) continue;
        seen.add(i);
        out.push(arr[i]);
    }
    return out;
}

function every(periodMs, fn) {
    timers.push(setInterval(fn, Math.max(5, Math.round(periodMs / RATE))));
}

function startScenarios() {
    log(`starting scenarios at RATE=${RATE}: ${EQUITIES.length} equities, `
        + `${OPTIONS.length} options, ${CRYPTO.length} crypto, ${FOREX.length} fx`);

    every(100,  () => { for (const s of pickN(EQUITIES, 10)) emit(stockTrade(s)); });
    every(50,   () => { for (const s of pickN(EQUITIES, 15)) emit(stockQuote(s)); });
    every(2000, () => { for (const s of EQUITIES) emit(stockMinuteAgg(s)); });

    every(400, () => { for (const s of pickN(OPTIONS, 6))  emit(optionTrade(s)); });
    every(200, () => { for (const s of pickN(OPTIONS, 12)) emit(optionQuote(s)); });

    every(120,  () => { for (const p of pickN(CRYPTO, 5)) emit(cryptoTrade(p)); });
    every(80,   () => { for (const p of pickN(CRYPTO, 7)) emit(cryptoQuote(p)); });
    every(2000, () => { for (const p of CRYPTO) emit(cryptoMinuteAgg(p)); });

    every(250,  () => { for (const p of FOREX) emit(forexQuote(p)); });
    every(3000, () => { for (const p of FOREX) emit(forexMinuteAgg(p)); });
}

reconnect();
