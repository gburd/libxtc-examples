// 16_l2book/web/console.js -- the l2book console's renderer.
//
// The renderer is intrepidkarthi/orderbook's web/console.js (3fffc25),
// reused under the MIT License (Copyright (c) 2026 Karthikeyan NG; the
// full notice is in console.html and web/README.md).  What changed:
//   - the market is book.wasm (16_l2book: book.c + console.c + signals.c,
//     compiled to wasm32 with no imports) instead of their obook.wasm.  A
//     small bridge below gives the renderer their obReset / obStep /
//     obSubmit / obSnapshot / obSignals / obAlerts / obCancel / obSpoof /
//     obFlood / obOpenOrders, as objects rather than JSON strings, over
//     book.wasm's con_* exports.  The page computes no signal itself;
//   - LIVE mode: served by a running l2book venue (/symbols answers), the
//     ladder and tape are the venue's book over SSE, "you" trades through
//     /order, and book.wasm's live_* exports run the same signal code over
//     what the venue publishes.
(async function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  let W;
  try {
    let result;
    try {
      result = await WebAssembly.instantiateStreaming(fetch("book.wasm"), {});
    } catch {
      const bytes = await (await fetch("book.wasm")).arrayBuffer();
      result = await WebAssembly.instantiate(bytes, {});
    }
    W = result.instance.exports;
    if (W.con_reset(1n) !== 0) throw new Error("con_reset");
  } catch (err) {
    $("boot").textContent =
      "The engine failed to load (" + err + "). This page needs WebAssembly and fetch -- " +
      "a current Firefox, Chrome, Safari or Edge.";
    return;
  }
  $("boot").hidden = true;
  $("ui").hidden = false;

  // ---- the bridge: their obXxx, over book.wasm --------------------------
  const DOT = "\u25CF", DASH = "\u2014";
  const YOU = 1;
  const who = (a) => a === 1 ? "you" : a === 2 ? "spoofer" : a === 3 ? "flooder" :
    a >= 100 ? "flow-" + (a - 100) : "account-" + a;
  const dv = () => new DataView(W.memory.buffer);
  // An integer count of 10^-dp units as Go's decimal prints it (trailing
  // zeros trimmed): 10000 ticks -> "100", 420 lots -> "0.42".
  function decStr(n, dp) {
    const neg = n < 0, s = String(neg ? -n : n).padStart(dp + 1, "0");
    let ip = s.slice(0, s.length - dp), fp = dp ? s.slice(s.length - dp) : "";
    fp = fp.replace(/0+$/, "");
    return (neg ? "-" : "") + ip + (fp ? "." + fp : "");
  }
  // A decimal string to integer units of 10^-dp, rounded half away from
  // zero (Instrument.PriceToTicks / QtyToLots); null if it is not a number.
  function parseDec(str, dp) {
    const m = /^\s*([+-]?)(\d*)(?:\.(\d*))?(?:[eE]([+-]?\d+))?\s*$/.exec(String(str));
    if (!m || (m[2] + (m[3] || "")) === "") return null;
    const fr = m[3] || "", pow = dp + (m[4] ? parseInt(m[4], 10) : 0) - fr.length;
    if (Math.abs(pow) > 40) return null;
    let v = BigInt((m[2] || "0") + fr);
    if (pow >= 0) v *= 10n ** BigInt(pow);
    else { const d = 10n ** BigInt(-pow); v = (v * 2n + d) / (2n * d); }
    if (v > (1n << 62n)) return null;
    return m[1] === "-" ? -v : v;
  }
  const REASON = {
    1: "duplicate order id", 2: "order not found", 3: "invalid quantity: must be positive",
    4: "invalid price: must be positive", 5: "post-only order would cross the spread",
    6: "FOK order cannot be fully filled", 7: "market order cannot be filled: no liquidity",
    8: "not accepted in this session phase", 9: "price is outside the allowed band",
    10: "order book has reached maximum capacity",
  };
  function readTrades() {
    const n = W.con_ntrades(), p = W.con_trades(), d = dv(), out = [];
    for (let i = 0; i < n; i++) {
      const o = p + i * 24;
      out.push({ px: Number(d.getBigInt64(o, true)), q: Number(d.getBigInt64(o + 8, true)),
        sold: d.getUint32(o + 16, true) !== 0 });
    }
    return out.map((t) => ({ tick: t.px, price: decStr(t.px, 2), quantity: decStr(t.q, 3),
      taker_side: t.sold ? "SELL" : "BUY" }));
  }
  function readSnap(p) {
    const d = dv(), nb = d.getUint32(p, true), na = d.getUint32(p + 4, true);
    const lv = (off, n) => Array.from({ length: n }, (_, i) =>
      ({ tick: Number(d.getBigInt64(p + off + 8 * i, true)), lots: Number(d.getBigInt64(p + off + 80 + 8 * i, true)) }));
    return { bids: lv(8, nb), asks: lv(168, na) };
  }
  function readSigs(p) {
    const d = dv();
    return { step: Number(d.getBigInt64(p, true)), has_mid: d.getInt32(p + 8, true) !== 0,
      mid: Number(d.getBigInt64(p + 16, true)), spread: Number(d.getBigInt64(p + 24, true)),
      last: Number(d.getBigInt64(p + 32, true)), ofi: Number(d.getBigInt64(p + 40, true)),
      cvd: Number(d.getBigInt64(p + 48, true)), imbalance: d.getFloat64(p + 56, true),
      lambda: { lambda: d.getFloat64(p + 72, true), r2: d.getFloat64(p + 88, true), n: d.getUint32(p + 96, true) } };
  }
  const SIM = {
    reset: (seed) => W.con_reset(BigInt(Math.trunc(seed) || 0)),
    step: (n) => { W.con_run(n); return { trades: readTrades() }; },
    submit(user, side, type, price, qty) {
      const market = type === "MARKET";
      const p = market ? 0n : parseDec(price, 2), q = parseDec(qty, 3);
      if (q === null) return { error: "bad quantity" };
      if (p === null) return { error: REASON[4] };
      const r = W.con_order(YOU, side === "SELL" ? 1 : 0, market ? 1 : 0, p, q);
      const res = { id: Number(W.con_order_id()), trades: readTrades() };
      if (r) res.error = REASON[r] || "refused (reason " + r + ")";
      return res;
    },
    snapshot() {
      const s = readSnap(W.con_book()), g = readSigs(W.con_sigs());
      return { bids: s.bids, asks: s.asks, mid: g.has_mid ? decStr(g.mid, 2) : "",
        spread: g.has_mid ? decStr(g.spread, 2) : "", last_trade: decStr(g.last, 2) };
    },
    signals() {
      const g = readSigs(W.con_sigs());
      return { step: g.step, // The first 12 hex digits, as their bar shows.
        digest: BigInt.asUintN(64, W.con_digest()).toString(16).padStart(16, "0").slice(0, 12),
        mid: g.has_mid ? g.mid / 100 : 0, spread: g.has_mid ? g.spread / 100 : 0, last: g.last / 100,
        ofi: g.ofi / 1000, cvd: g.cvd / 1000, imbalance: g.imbalance,
        // The fit is per lot (0.001); per 1.0 displayed quantity, as theirs.
        lambda: { lambda: g.lambda.lambda * 1000, r2: g.lambda.r2, n: g.lambda.n } };
    },
    alerts(since) {
      const total = Number(W.con_nalerts()), out = [];
      for (let i = Math.max(since, 0); i < total; i++) {
        const p = W.con_alert(BigInt(i));
        if (!p) continue;
        const d = dv(), kind = d.getUint8(p), seq = d.getBigUint64(p + 8, true);
        const a = Number(d.getBigInt64(p + 16, true)), b = Number(d.getBigInt64(p + 24, true)), c = Number(d.getBigInt64(p + 32, true));
        out.push(kind === 1 ?
          { kind: "spoofing", user: who(d.getUint32(p + 4, true)), seq,
            detail: `order ${a} (size ${b}) cancelled unfilled ${c} events after placement` } :
          { kind: "order_to_trade_ratio", user: who(d.getUint32(p + 4, true)), seq,
            detail: `${a} orders / ${b} fills = OTR ${d.getFloat64(p + 40, true).toFixed(1)} within ${c} events (limit 15.0)` });
      }
      return { alerts: out, total };
    },
    cancel: (id) => W.con_pull(BigInt(id), YOU) === 0 ? {} : { error: "order not found" },
    spoof: () => ({ [-1]: { error: "a spoof is already resting" }, [-2]: { error: "no bid to spoof behind yet" },
      [-3]: { error: "spoof orders were rejected" } })[W.con_do_spoof()] || { ok: true },
    flood: () => W.con_do_flood() === 0 ? { ok: true } : { error: "no book to flood yet" },
    openOrders() {
      const n = W.con_open(YOU), d = dv(), out = [];
      for (let i = 0; i < n; i++)
        out.push({ id: Number(d.getBigUint64(W.con_open_id() + 8 * i, true)),
          side: d.getBigInt64(W.con_open_side() + 8 * i, true) ? "SELL" : "BUY",
          tick: Number(d.getBigInt64(W.con_open_px() + 8 * i, true)),
          qty: decStr(Number(d.getBigInt64(W.con_open_q() + 8 * i, true)), 3) });
      return { orders: out };
    },
    lotDp: 3,
  };

  // ---- LIVE: the venue's book over SSE, the same signals over it ---------
  const LIVE = {
    sym: "", es: null, bids: new Map(), asks: new Map(), pending: [], last: 0, seq: 0, step: 0, sig: null,
    start(sym) {
      this.stop(); this.sym = sym; this.bids.clear(); this.asks.clear(); this.pending = [];
      this.last = 0; this.seq = 0; this.step = 0; this.sig = null; W.live_reset();
      const es = this.es = new EventSource("md?sym=" + encodeURIComponent(sym));
      es.addEventListener("book", (e) => { const m = JSON.parse(e.data); this.bids.clear(); this.asks.clear(); if (m.last > 0) this.last = m.last; this.seq = m.seq; });
      es.addEventListener("level", (e) => { const m = JSON.parse(e.data); const side = m.s ? this.asks : this.bids; m.q > 0 ? side.set(m.p, m.q) : side.delete(m.p); this.seq = m.seq; });
      es.addEventListener("trade", (e) => { const m = JSON.parse(e.data); this.last = m.p; this.seq = m.seq; W.live_trade(m.s ? 1 : 0, BigInt(m.q)); this.pending.push({ tick: m.p, price: (m.p / 100).toFixed(2), quantity: String(m.q), taker_side: m.s ? "SELL" : "BUY" }); });
      es.onerror = () => { $("mv-label").textContent = "LIVE VENUE \u00B7 RECONNECTING"; };
      es.onopen = () => { $("mv-label").textContent = "LIVE VENUE \u00B7 SSE"; };
    },
    stop() { if (this.es) this.es.close(); this.es = null; },
    top(map, desc) { return [...map.entries()].sort((a, b) => desc ? b[0] - a[0] : a[0] - b[0]).slice(0, 10).map(([tick, lots]) => ({ tick, lots })); },
    // One 100 ms sampling interval: write the top 10 into book.wasm, run the signals.
    sample() {
      const bids = this.top(this.bids, true), asks = this.top(this.asks, false), p = W.live_snap(), d = dv();
      d.setUint32(p, bids.length, true); d.setUint32(p + 4, asks.length, true);
      bids.forEach((l, i) => { d.setBigInt64(p + 8 + 8 * i, BigInt(l.tick), true); d.setBigInt64(p + 88 + 8 * i, BigInt(l.lots), true); });
      asks.forEach((l, i) => { d.setBigInt64(p + 168 + 8 * i, BigInt(l.tick), true); d.setBigInt64(p + 248 + 8 * i, BigInt(l.lots), true); });
      this.sig = readSigs(W.live_tick()); this.step++;
      this.snap = { bids, asks };
      const t = this.pending; this.pending = [];
      return { trades: t };
    },
    snapshot() {
      const s = this.snap || { bids: [], asks: [] }, g = this.sig;
      return { bids: s.bids, asks: s.asks, mid: g && g.has_mid ? decStr(g.mid, 2) : "",
        spread: g && g.has_mid ? decStr(g.spread, 2) : "", last_trade: decStr(this.last, 2) };
    },
    signals() {
      const g = this.sig || { has_mid: false, ofi: 0, cvd: 0, imbalance: 0, lambda: { n: 0 } };
      return { step: this.step, digest: String(this.seq), mid: g.has_mid ? g.mid / 100 : 0,
        spread: g.has_mid ? g.spread / 100 : 0, last: this.last / 100, ofi: g.ofi, cvd: g.cvd,
        imbalance: g.imbalance, lambda: g.lambda };
    },
    async submit(user, side, type, price, qty) {
      const market = type === "MARKET", p = market ? 0n : parseDec(price, 2), q = parseDec(qty, 0);
      if (q === null || q <= 0n) return { error: "invalid quantity: must be positive" };
      if (p === null || (!market && p <= 0n)) return { error: REASON[4] };
      const qs = new URLSearchParams({ sym: this.sym, side: side === "SELL" ? "s" : "b", type: market ? "m" : "l",
        tif: market ? "i" : "g", price: String(p), qty: String(q) });
      try {
        const r = await (await fetch("order?" + qs, { method: "POST" })).json();
        return r.ok ? { id: r.id, trades: [] } : { error: "the venue did not take it" };
      } catch (e) { return { error: "the venue did not answer (" + e + ")" }; }
    },
    lotDp: 0,
  };

  // ---- state ----------------------------------------------------------------
  const TICK_MS = 100;          // 10 steps/second at 1x
  const RING = 300;             // sparkline history
  const TAPE_MAX = 60;
  let running = !matchMedia("(prefers-reduced-motion: reduce)").matches;
  let speed = 1;
  let alertCount = 0;
  let mode = "sim";
  const seedParam = new URLSearchParams(location.search).get("seed");
  const initialSeed = seedParam && Number.isFinite(+seedParam) ? +seedParam : null;
  if (initialSeed != null) {
    SIM.reset(initialSeed);
    $("c-seed").value = initialSeed;
  }
  let yourPrices = new Set();   // ticks where "you" rest, for the ladder markers
  const mids = [], ofis = [], cvds = [];

  const push = (arr, v) => { arr.push(v); if (arr.length > RING) arr.shift(); };
  const fmtP = (v) => v ? v.toFixed(2) : DASH;
  const fmtS = (v, dp = 2) => (v > 0 ? "+" : "") + v.toFixed(dp);
  const syncSeedURL = (seed) => history.replaceState(null, "", `?seed=${encodeURIComponent(seed)}`);
  const M = () => mode === "sim" ? SIM : LIVE;

  if (!running) $("c-run").textContent = "Run";

  // ---- sparklines (theirs) --------------------------------------------------
  // 2px single-hue line on a recessive (grid-free) surface; the current value
  // is direct-labeled in the panel header, hover shows the value under the
  // cursor. Identity comes from the panel title, so no legend.
  function drawSpark(canvas, series, hoverX) {
    const dpr = devicePixelRatio || 1;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    if (canvas.width !== w * dpr) { canvas.width = w * dpr; canvas.height = h * dpr; }
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    if (series.length < 2) return null;

    let min = Math.min(...series), max = Math.max(...series);
    if (max - min < 1e-9) { max += 1; min -= 1; }
    const pad = (max - min) * 0.12;
    min -= pad; max += pad;
    const x = (i) => (i / (series.length - 1)) * (w - 10) + 5;
    const y = (v) => h - 6 - ((v - min) / (max - min)) * (h - 12);

    const css = getComputedStyle(document.documentElement);
    const line = css.getPropertyValue("--link").trim() || "#58a6ff";
    const muted = css.getPropertyValue("--muted").trim() || "#8b949e";

    ctx.strokeStyle = line; ctx.lineWidth = 2; ctx.lineJoin = "round"; ctx.beginPath();
    series.forEach((v, i) => (i ? ctx.lineTo(x(i), y(v)) : ctx.moveTo(x(i), y(v))));
    ctx.stroke();
    ctx.fillStyle = line; ctx.beginPath();
    ctx.arc(x(series.length - 1), y(series[series.length - 1]), 3, 0, 7);
    ctx.fill();

    if (hoverX != null) {
      const i = Math.max(0, Math.min(series.length - 1, Math.round(((hoverX - 5) / (w - 10)) * (series.length - 1))));
      ctx.strokeStyle = muted; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(x(i), 4); ctx.lineTo(x(i), h - 4); ctx.stroke();
      return series[i];
    }
    return null;
  }

  const sparks = [
    { canvas: $("sp-mid"), series: mids, cur: $("cur-mid"), fmt: fmtP },
    { canvas: $("sp-ofi"), series: ofis, cur: $("cur-ofi"), fmt: (v) => fmtS(v, 1) },
    { canvas: $("sp-cvd"), series: cvds, cur: $("cur-cvd"), fmt: (v) => fmtS(v, 1) },
  ];
  for (const s of sparks) {
    s.hoverX = null;
    s.canvas.addEventListener("mousemove", (e) => { s.hoverX = e.offsetX; renderSpark(s); });
    s.canvas.addEventListener("mouseleave", () => { s.hoverX = null; renderSpark(s); });
  }
  function renderSpark(s) {
    const hv = drawSpark(s.canvas, s.series, s.hoverX);
    const last = s.series[s.series.length - 1];
    if (last !== undefined) s.cur.textContent = s.fmt(hv != null ? hv : last);
  }

  // ---- ladder ---------------------------------------------------------------
  let flashPx = null;
  function renderLadder() {
    const snap = M().snapshot(), dp = M().lotDp, f = 10 ** dp;
    const rows = [];
    const maxSz = Math.max(1e-9, ...snap.asks.map((l) => l.lots), ...snap.bids.map((l) => l.lots));
    const row = (cls, l) => {
      const flash = flashPx === l.tick ? " flash" : "";
      const yours = yourPrices.has(l.tick) ? `<span class="own-dot" title="your resting order">${DOT}</span>` : "";
      return `<div class="lrow ${cls}${flash}"><span class="bar" style="width:${(100 * l.lots) / maxSz}%"></span>` +
        `<span class="p">${yours}${(l.tick / 100).toFixed(2)}</span><span class="q">${(l.lots / f).toFixed(dp)}</span></div>`;
    };
    rows.push(`<div class="lsec">ASKS \u00B7 price / size</div>`);
    for (const l of [...snap.asks].reverse()) rows.push(row("ask", l));  // best ask adjacent to mid
    rows.push(`<div class="lmid"><span>mid <b>${snap.mid || DASH}</b></span><span>spread <b>${snap.spread || DASH}</b></span><span>last <b>${snap.last_trade}</b></span></div>`);
    for (const l of snap.bids) rows.push(row("bid", l));
    rows.push(`<div class="lsec">BIDS</div>`);
    $("ladder").innerHTML = rows.join("");
    flashPx = null;
    return snap;
  }

  // ---- your orders ----------------------------------------------------------
  const NOTHING = `<div class="tape-empty">Nothing resting. A limit order away from the touch will sit in the book ${DASH} marked ${DOT} on the ladder.</div>`;
  function renderOwn() {
    const el = $("own");
    if (mode !== "sim") {
      yourPrices = new Set();
      el.innerHTML = `<div class="tape-empty">LIVE: your orders go to the venue as account 1 (POST /order). ` +
        `The venue's web port has no open-orders or cancel endpoint (PROTOCOL.md), so they are not listed here.</div>`;
      return;
    }
    const res = SIM.openOrders();
    yourPrices = new Set(res.orders.map((o) => o.tick));
    if (!res.orders.length) { el.innerHTML = NOTHING; return; }
    el.innerHTML = res.orders.map((o) =>
      `<div class="orow"><span class="oside">${o.side === "BUY" ? "B" : "S"}</span>` +
      `<span class="px" style="color:var(--${o.side === "BUY" ? "bid" : "ask"})">${(o.tick / 100).toFixed(2)}</span>` +
      `<span class="oq">\u00D7 ${o.qty}</span>` +
      `<button class="ox" data-id="${o.id}" title="cancel (book_cancel)" aria-label="Cancel order ${o.id}">\u2715</button></div>`
    ).join("");
    el.querySelectorAll(".ox").forEach((b) =>
      b.addEventListener("click", () => { SIM.cancel(+b.dataset.id); refresh(); })
    );
  }

  // ---- tape -----------------------------------------------------------------
  const tapeEl = $("tape");
  let tapeCount = 0;
  function appendTrades(trades) {
    if (!trades || !trades.length) return;
    if (tapeCount === 0) tapeEl.innerHTML = "";
    const big = mode === "sim" ? 2 : 1000;
    for (const t of trades) {
      const buy = t.taker_side === "BUY";
      const div = document.createElement("div");
      div.className = "trow " + (buy ? "b" : "s") + (+t.quantity >= big ? " big" : "");
      div.innerHTML = `<span class="side">${buy ? "\u25B2 B" : "\u25BC S"}</span><span class="px">${t.price}</span><span class="qy">${t.quantity}</span>`;
      tapeEl.prepend(div);
      tapeCount++;
      flashPx = t.tick;
    }
    while (tapeEl.children.length > TAPE_MAX) tapeEl.lastChild.remove();
  }

  // ---- alerts ---------------------------------------------------------------
  function drainAlerts() {
    if (mode !== "sim") return;
    const res = SIM.alerts(alertCount);
    if (!res.alerts.length) return;
    if (alertCount === 0) $("alist").innerHTML = "";
    for (const a of res.alerts) {
      const div = document.createElement("div");
      div.className = "arow";
      div.innerHTML = `<span class="chip">${a.kind}</span><span class="who">${a.user}</span><span class="det">${a.detail} (seq ${a.seq})</span>`;
      $("alist").prepend(div);
    }
    alertCount = res.total;
    $("cur-alerts").textContent = `${alertCount} alert${alertCount === 1 ? "" : "s"}`;
  }

  // ---- signals --------------------------------------------------------------
  function renderSignals() {
    const s = M().signals();
    $("mv-mid").textContent = fmtP(s.mid);
    $("mv-spread").textContent = fmtP(s.spread);
    $("mv-last").textContent = fmtP(s.last);
    $("mv-step").textContent = s.step;
    $("mv-digest").textContent = s.digest || DASH;
    if (s.mid) push(mids, s.mid);
    push(ofis, s.ofi);
    push(cvds, s.cvd);
    sparks.forEach(renderSpark);

    const imb = s.imbalance || 0;
    $("cur-imb").textContent = fmtS(imb);
    const fill = $("g-fill");
    fill.className = "fill " + (imb >= 0 ? "pos" : "neg");
    fill.style.width = Math.min(50, Math.abs(imb) * 50) + "%";

    if (s.lambda && s.lambda.n) {
      $("lam-v").textContent = s.lambda.lambda.toFixed(3);
      $("lam-r2").textContent = `R\u00B2 ${s.lambda.r2.toFixed(2)} \u00B7 n=${s.lambda.n}`;
    }
  }

  function refresh() {
    renderOwn();
    renderLadder();
    renderSignals();
    drainAlerts();
  }

  // ---- main loop ------------------------------------------------------------
  function tick() {
    if (mode !== "sim") {
      const out = LIVE.sample();          // sample even when paused: the signals stay honest
      if (!running || document.hidden) return;
      appendTrades(out.trades);
      refresh();
      return;
    }
    if (!running || document.hidden) return;
    const out = SIM.step(speed);
    appendTrades(out.trades);
    refresh();
  }
  setInterval(tick, TICK_MS);
  refresh();

  // ---- controls -------------------------------------------------------------
  function clearPanels(emptyAlerts) {
    mids.length = ofis.length = cvds.length = 0;
    tapeEl.innerHTML = '<div class="tape-empty">No prints yet.</div>';
    tapeCount = 0;
    alertCount = 0;
    $("alist").innerHTML = emptyAlerts;
    $("cur-alerts").textContent = "0 alerts";
    $("lam-v").textContent = DASH;
    $("lam-r2").textContent = "warming up";
  }
  const QUIET = `<div class="surv-empty">Quiet ${DASH} the detector is watching every event this page generates. Provoke it: <b>Run a spoof</b>.</div>`;
  $("c-run").addEventListener("click", () => {
    running = !running;
    $("c-run").textContent = running ? "Pause" : "Run";
  });
  $("c-speed").addEventListener("change", (e) => { speed = +e.target.value; });
  $("c-reset").addEventListener("click", () => {
    const seed = +($("c-seed").value || 1);
    syncSeedURL(seed);
    SIM.reset(seed);
    clearPanels(QUIET);
    refresh();
  });

  $("c-share").addEventListener("click", async () => {
    const seed = +($("c-seed").value || 1);
    syncSeedURL(seed);
    const button = $("c-share");
    try {
      await navigator.clipboard.writeText(location.href);
      button.textContent = "Copied";
      setTimeout(() => { button.textContent = "Copy link"; }, 1800);
    } catch {
      button.textContent = "Copy failed";
      setTimeout(() => { button.textContent = "Copy link"; }, 1800);
    }
  });

  const showErr = (msg) => {
    $("t-err").textContent = msg;
    setTimeout(() => { if ($("t-err").textContent === msg) $("t-err").textContent = ""; }, 5000);
  };
  async function trade(side, type, price, qty) {
    const res = await M().submit("you", side, type, price, qty);
    if (res.error) showErr(res.error);
    appendTrades(res.trades);
    refresh();
    return res;
  }
  $("trade-form").addEventListener("submit", (e) => {
    e.preventDefault();
    trade($("t-side").value, "LIMIT", $("t-price").value, $("t-qty").value);
  });
  $("c-buy").addEventListener("click", () => trade("BUY", "MARKET", "0", $("t-qty").value || "1.0"));
  $("c-sell").addEventListener("click", () => trade("SELL", "MARKET", "0", $("t-qty").value || "1.0"));

  // A provocation button disables until its detector has actually spoken (new
  // alerts beyond the count at press time), with a timeout so a failed attempt
  // (e.g. an empty book) cannot wedge the button.
  function provoke(btn, restLabel, busyLabel, call) {
    btn.addEventListener("click", () => {
      if (mode !== "sim") return;
      const baseline = alertCount;
      const res = call();
      if (res.error) return;
      if (!running) {
        const out = SIM.step(3);
        appendTrades(out.trades);
      }
      refresh();
      btn.disabled = true;
      btn.textContent = busyLabel;
      const started = Date.now();
      const iv = setInterval(() => {
        if (alertCount > baseline || Date.now() - started > 12000) {
          btn.disabled = false;
          btn.textContent = restLabel;
          clearInterval(iv);
        }
      }, 300);
    });
  }
  provoke($("c-spoof"), "Run a spoof", "Spoof resting\u2026", () => SIM.spoof());
  provoke($("c-flood"), "Run a flood", "Flooding\u2026", () => SIM.flood());

  // ---- LIVE / SIM -----------------------------------------------------------
  function setMode(v) {
    LIVE.stop();
    mode = v === "sim" ? "sim" : "live";
    const live = mode === "live";
    document.querySelectorAll(".sim-only").forEach((el) => { el.hidden = live; });
    $("c-spoof").disabled = $("c-flood").disabled = live;
    $("mv-sym").textContent = live ? v.slice(5) : "DEMO";
    $("mv-label").textContent = live ? "LIVE VENUE \u00B7 SSE" : "SIMULATED FLOW \u00B7 REAL ENGINE";
    $("mv-digest").parentNode.firstChild.nodeValue = live ? "seq" : "digest";
    // SIM sizes are 0.001 lots, displayed per 1.0 (theirs); LIVE sizes are shares.
    document.querySelector(".lam .u").textContent = live ? "ticks / 1 share" : "ticks / 1.0 qty";
    $("t-qty").value = live ? "100" : "1.0";
    $("t-price").value = "";
    clearPanels(live ?
      `<div class="surv-empty">LIVE: the venue's market data is level 2 (price levels and prints, no order ids), ` +
      `so the order-level detectors have nothing to watch here. They run in SIM, on every event.</div>` : QUIET);
    if (live) LIVE.start(v.slice(5));
    refresh();
  }
  $("c-mode").addEventListener("change", (e) => setMode(e.target.value));
  // Served by a running venue, /symbols lists its books: offer LIVE, and
  // start in it unless the URL asks for a seed.  A static page has none.
  try {
    const r = await fetch("symbols", { cache: "no-store" });
    const list = r.ok ? await r.json() : null;
    if (Array.isArray(list) && list.length) {
      const sel = $("c-mode");
      sel.innerHTML = list.map((s) => `<option value="live:${s}">LIVE ${s}</option>`).join("") +
        `<option value="sim">SIM (book.wasm)</option>`;
      sel.hidden = false;
      $("fp-live").hidden = false;
      if (initialSeed == null) { sel.value = "live:" + list[0]; setMode(sel.value); }
      else sel.value = "sim";
    }
  } catch { /* no venue behind this page: SIM only */ }
})();
