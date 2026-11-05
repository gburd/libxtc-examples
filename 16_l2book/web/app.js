// 16_l2book/web/app.js -- the landing page's two live illustrations, run on
// book.wasm (the engine, book.c): a real order-book ladder, and a
// step-by-step matching walkthrough.  No faked data.
//
// The illustrations, their text and their rendering are intrepidkarthi/
// orderbook's web/app.js (3fffc25), reused under the MIT License (Copyright
// (c) 2026 Karthikeyan NG; the full notice is in web/README.md and
// style.css).  What changed: the engine underneath is book.wasm's lob_*
// exports (one bare book), and sizes are whole shares.
"use strict";

let E;                  // book.wasm exports
let nextId = 1;

async function boot() {
  let result;
  try {
    result = await WebAssembly.instantiateStreaming(fetch("book.wasm"), {});
  } catch (_) {
    const bytes = await (await fetch("book.wasm")).arrayBuffer();
    result = await WebAssembly.instantiate(bytes, {});
  }
  E = result.instance.exports;
  renderBasics();
  wireMatching();
  document.getElementById("loading").classList.add("done");
}

// ---- the engine: a fresh book, orders, its depth, the prints ----------
const EV_TRADE = 2, EV_SIZE = 56;   // BOOK_EV_TRADE; sizeof(book_event_t)
const px = (t) => (t / 100).toFixed(2);

function resetBook() {
  E.lob_init(10000n, 1 << 10);
  nextId = 1;
}

// submit(account, "BUY" | "SELL", "LIMIT" | "MARKET", price, qty) -> trades
function submit(acct, side, type, price, qty) {
  const market = type === "MARKET";
  E.lob_submit(BigInt(nextId++), acct, side === "SELL" ? 1 : 0, market ? 1 : 0, market ? 1 : 0, 0,
    BigInt(Math.round(price * 100)), 0n, BigInt(qty), 0n);
  const n = E.lob_ev_count(), base = E.lob_ev(), d = new DataView(E.memory.buffer), out = [];
  for (let i = 0; i < n; i++) {
    const o = base + i * EV_SIZE;
    if (d.getUint8(o) === EV_TRADE)
      out.push({ price: px(Number(d.getBigInt64(o + 24, true))), quantity: Number(d.getBigInt64(o + 32, true)) });
  }
  return out;
}

function snapshot(depth) {
  const side = (s) => {
    const n = E.lob_depth(s, depth), d = new DataView(E.memory.buffer), p = E.lob_depth_px(), q = E.lob_depth_q(), out = [];
    for (let i = 0; i < n; i++)
      out.push({ price: px(Number(d.getBigInt64(p + 8 * i, true))), size: String(d.getBigInt64(q + 8 * i, true)) });
    return out;
  };
  const bids = side(0), asks = side(1), bb = Number(E.lob_best(0)), ba = Number(E.lob_best(1));
  const two = bids.length && asks.length, last = E.lob_trades() > 0n ? px(Number(E.lob_last())) : "";
  return { bids, asks, spread: two ? px(ba - bb) : "", mid: two ? px(Math.floor((bb + ba) / 2)) : "", last_trade: last };
}

// ---------- shared ladder renderer (theirs) ----------
function buildSide(el, levels, side, maxS, bestIdx, hits, animate) {
  el.className = "ob-side " + (side === "ask" ? "a" : "b");
  el.innerHTML = levels
    .map((l, i) => {
      const w = ((parseFloat(l.size) / maxS) * 100).toFixed(1);
      const cls = "ob-row" + (i === bestIdx ? " best" : "") + (hits && hits[l.price] ? " hit" : "");
      const barW = animate ? "0" : w;
      return (
        '<div class="' + cls + '"><span class="p">' + l.price + '</span><span class="s">' +
        l.size + '</span><span class="bar" data-w="' + w + '" style="width:' + barW + '%"></span></div>'
      );
    })
    .join("");
  if (animate) {
    requestAnimationFrame(() => el.querySelectorAll(".bar").forEach((b) => (b.style.width = b.dataset.w + "%")));
  }
}

function renderLadder(cfg) {
  const s = cfg.snap;
  if (!s.asks.length && !s.bids.length) return;
  const asksDisp = s.asks.slice().reverse(); // worst -> best, so best ask sits by the spread
  const maxS = Math.max(1, ...s.asks.map((a) => +a.size), ...s.bids.map((b) => +b.size));
  buildSide(cfg.asksEl, asksDisp, "ask", maxS, asksDisp.length - 1, cfg.hits, cfg.animate);
  buildSide(cfg.bidsEl, s.bids, "bid", maxS, 0, cfg.hits, cfg.animate);
  if (cfg.spreadEl) cfg.spreadEl.textContent = "spread " + (s.spread || "\u2014");
  if (cfg.midEl) cfg.midEl.textContent = "mid " + (s.mid || "\u2014");
  if (cfg.lastEl) cfg.lastEl.textContent = "last " + (s.last_trade || "\u2014");
}

function submitMany(list) {
  list.forEach((o, i) => submit(i + 10, o[0], "LIMIT", o[1], o[2]));
}

// ---------- 1) basics ladder (static, real snapshot) ----------
function renderBasics() {
  resetBook();
  submitMany([
    ["SELL", 102, 5], ["SELL", 101.5, 2], ["SELL", 101, 3],
    ["BUY", 100, 4], ["BUY", 99.5, 3], ["BUY", 99, 6],
  ]);
  renderLadder({
    asksEl: document.getElementById("ladderAsks"),
    bidsEl: document.getElementById("ladderBids"),
    spreadEl: document.getElementById("obSpread"),
    midEl: document.getElementById("obMid"),
    snap: snapshot(8),
    animate: true,
  });
}

// ---------- 2) matching walkthrough ----------
let mIdx = -1;

function seedMatching() {
  resetBook();
  submitMany([
    ["SELL", 100.5, 2], ["SELL", 101, 3], ["SELL", 101.5, 4],
    ["BUY", 100, 3], ["BUY", 99.5, 2], ["BUY", 99, 4],
  ]);
  document.getElementById("mTape").innerHTML = "";
}

function renderMatching(hits) {
  renderLadder({
    asksEl: document.getElementById("mAsks"),
    bidsEl: document.getElementById("mBids"),
    spreadEl: document.getElementById("mSpread"),
    lastEl: document.getElementById("mLast"),
    snap: snapshot(8),
    hits,
  });
}

function sendMarket(qty) {
  const trades = submit(1, "BUY", "MARKET", 0, qty);
  const hits = {};
  const tape = document.getElementById("mTape");
  trades.forEach((t) => {
    hits[t.price] = 1;
    const el = document.createElement("div");
    el.className = "t buy";
    el.innerHTML = "<span>\u25B2 bought " + t.quantity + " @ " + t.price + "</span><span></span>";
    tape.prepend(el);
  });
  while (tape.children.length > 8) tape.lastChild.remove();
  return hits;
}

const mSteps = [
  {
    text: "Here's a book of resting orders \u2014 sellers (asks) on top, buyers (bids) below. Nobody has traded yet; these makers are just offering liquidity.",
    run: () => { seedMatching(); renderMatching(); },
  },
  {
    text: "A trader wants in now, so they send a MARKET BUY for 2. It takes the cheapest ask (the maker at the top) \u2014 a trade prints, at the maker's price.",
    run: () => renderMatching(sendMarket(2)),
  },
  {
    text: "A bigger MARKET BUY for 6 can't fill at one level \u2014 it walks up through several asks. Each deeper level is pricier, so the average fill is worse. That's slippage.",
    run: () => renderMatching(sendMarket(6)),
  },
  {
    text: "The best ask has climbed as liquidity was consumed \u2014 the book just discovered a new price. That's the whole job: turn resting intentions into trades and prices. \u21BB Restart to replay.",
    run: () => renderMatching(),
  },
];

function wireMatching() {
  const btn = document.getElementById("stepBtn");
  const reset = document.getElementById("stepReset");
  const txt = document.getElementById("stepText");
  seedMatching();
  renderMatching();

  btn.addEventListener("click", () => {
    mIdx = mIdx >= mSteps.length - 1 ? 0 : mIdx + 1;
    mSteps[mIdx].run();
    txt.textContent = mSteps[mIdx].text;
    btn.textContent = mIdx >= mSteps.length - 1 ? "\u21BB Restart" : "Next step \u2192";
  });
  reset.addEventListener("click", () => {
    mIdx = -1;
    seedMatching();
    renderMatching();
    txt.textContent = "Press Start to build a book, then walk through a trade step by step.";
    btn.textContent = "\u25B6 Start";
  });
}

boot();
