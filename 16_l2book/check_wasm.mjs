// 16_l2book/check_wasm.mjs -- book.wasm against known answers, in node.
//
//   node check_wasm.mjs [web/book.wasm]        (make check-wasm)
//
// The Pages workflow runs it before it deploys; a failure fails the deploy.
//   1. the bare engine (lob_*): the quickstart -- one trade, a two-sided book;
//   2. the console market (con_*): seed 1, 79 steps, is the market
//      intrepidkarthi/orderbook's console shows for seed 1 at step 79
//      (test_signals.c has the native twin of this case);
//   3. the console's step loop: 20,000 steps, book_check clean, the digest
//      the same twice for a seed and different for another; the spoof and
//      flood raise their alerts;
//   4. LIVE signals (live_*): OFI, CVD and imbalance over two given books;
//   5. any price rests: "you" place a sell at $900.00 and a buy at one cent
//      in the console's market (far outside the first window), the market
//      runs on, the book stays sound, and both orders are still there.
import { readFileSync } from 'node:fs';

const path = process.argv[2] || new URL('web/book.wasm', import.meta.url);
const { instance } = await WebAssembly.instantiate(readFileSync(path));
const w = instance.exports;
const fail = (m) => { console.error('check_wasm: FAIL ' + m); process.exit(1); };
const dv = () => new DataView(w.memory.buffer);
const sigs = (p) => { const d = dv(); return { step: Number(d.getBigInt64(p, true)), mid: Number(d.getBigInt64(p + 16, true)),
  spread: Number(d.getBigInt64(p + 24, true)), ofi: Number(d.getBigInt64(p + 40, true)), cvd: Number(d.getBigInt64(p + 48, true)),
  imb: d.getFloat64(p + 56, true), lambda: d.getFloat64(p + 72, true), r2: d.getFloat64(p + 88, true), n: d.getUint32(p + 96, true) }; };
const near = (a, b) => Math.abs(a - b) < 1e-9;

// 1. lob_*: a resting sell, a crossing IOC buy, then a book with a bid and an ask.
if (w.lob_init(10000n, 1 << 15) !== 0) fail('lob_init');
w.lob_submit(1n, 1, 1, 0, 0, 0, 10100n, 0n, 6n, 0n);
w.lob_submit(2n, 2, 0, 0, 1, 0, 10100n, 0n, 5n, 0n);
w.lob_submit(3n, 3, 0, 0, 0, 0, 9900n, 0n, 10n, 0n);
if (Number(w.lob_trades()) !== 1) fail('lob trades ' + w.lob_trades());
if (Number(w.lob_best(0)) !== 9900 || Number(w.lob_best(1)) !== 10100) fail('lob bbo');
if (w.lob_check() !== 0) fail('lob_check');

// 2. con_*: their console, seed 1, step 79 (cmd/obwasm built and run headless).
if (w.con_reset(1n) !== 0) fail('con_reset');
let prints = 0;
for (let i = 0; i < 79; i++) prints += w.con_run(1);
let s = sigs(w.con_sigs());
if (prints !== 28 || s.step !== 79 || s.mid !== 10000 || s.spread !== 1 || s.ofi !== -6158 || s.cvd !== -606 ||
    !near(s.imb, -0.3374369169597798) || s.n !== 71 || !near(s.lambda * 1000, 0.21463660886548466) ||
    !near(s.r2, 0.06012877762957247))
  fail('seed 1 step 79: ' + JSON.stringify({ prints, ...s }));

// 3. the step loop: sound, replayable, provocations detected.
const run = (seed, steps, chunk) => {
  w.con_reset(BigInt(seed));
  for (let i = 0; i < steps; i += chunk) w.con_run(chunk);
  if (w.con_check() !== 0) fail('book_check ' + w.con_check() + ' seed ' + seed);
  return BigInt.asUintN(64, w.con_digest()).toString(16);
};
const d1 = run(42, 20000, 1), d2 = run(42, 20000, 200), d3 = run(43, 20000, 1);
if (d1 !== d2 || d1 === d3) fail(`replay: ${d1} ${d2} ${d3}`);
w.con_reset(1n);
w.con_run(200);
if (w.con_do_spoof() !== 0 || w.con_do_spoof() !== -1) fail('spoof');
w.con_run(5);
const spoofs = Number(w.con_nalerts());
if (spoofs !== 3 || dv().getUint8(w.con_alert(0n)) !== 1) fail('spoof alerts ' + spoofs);
if (w.con_do_flood() !== 0 || Number(w.con_nalerts()) !== spoofs + 6) fail('flood alerts ' + w.con_nalerts());

// 4. live_*: bid 100x5 / ask 101x5, then the bid grows to 8 (OFI +3, the
// ofi_test.go case), one buy print of 2 (CVD +2), imbalance (8-5)/13.
w.live_reset();
const put = (bq) => { const p = w.live_snap(), d = dv(); d.setUint32(p, 1, true); d.setUint32(p + 4, 1, true);
  d.setBigInt64(p + 8, 100n, true); d.setBigInt64(p + 88, BigInt(bq), true);
  d.setBigInt64(p + 168, 101n, true); d.setBigInt64(p + 248, 5n, true); };
put(5); w.live_tick();
put(8); w.live_trade(0, 2n);
s = sigs(w.live_tick());
if (s.ofi !== 3 || s.cvd !== 2 || !near(s.imb, 3 / 13) || s.mid !== 100) fail('live ' + JSON.stringify(s));

// 5. far prices: accepted (reason 0), the market runs 2,000 steps, both rest.
w.con_reset(7n);
w.con_run(100);
if (w.con_order(1, 1, 0, 90000n, 3n) !== 0) fail('far sell refused');
const farSell = w.con_order_id();
if (w.con_order(1, 0, 0, 1n, 2n) !== 0) fail('one-cent buy refused');
const farBuy = w.con_order_id();
w.con_run(2000);
if (w.con_check() !== 0) fail('book_check after far orders ' + w.con_check());
if (w.con_pull(farSell, 1) !== 0 || w.con_pull(farBuy, 1) !== 0) fail('far orders not resting');
if (w.con_check() !== 0) fail('book_check after cancels ' + w.con_check());

console.log(`check_wasm: ok (lob quickstart; seed 1 x79 = intrepidkarthi's console; ` +
  `seed 42 x20000 digest ${d1} twice, seed 43 differs; 3 spoof + 6 OTR alerts; live OFI/CVD/imbalance; ` +
  `far prices rest)`);
