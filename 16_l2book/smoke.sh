#!/bin/sh
# 16_l2book/smoke.sh -- bring the compose box up and check it, the way
# intrepidkarthi/orderbook's box.yml does:
#   1. the venue is ready (/readyz 200 within 120 s);
#   2. the books are quoted (every symbol has a bid and an ask);
#   3. the market trades (trades rise over 10 s), including the outside
#      flow container's orders;
#   4. the landing page, the console (console.html, console.js) and
#      book.wasm are served, the venue answers the console's /symbols probe
#      (LIVE mode), and its SSE stream carries level and trade events;
#   5. the venue restarts and comes back ready and quoted.
# Usage: smoke.sh [compose command]   (default: "docker compose";
#        "podman compose" works too).  Run from the libxtc checkout root.
set -eu
DC=${1:-docker compose}
F=examples/16_l2book/compose.yaml
URL=http://127.0.0.1:8080
fail() { echo "  [l2book-box] FAIL: $*"; $DC -f $F logs --tail 30 || true; $DC -f $F down || true; exit 1; }
trades() { wget -qO- $URL/stats | sed 's/"trades":/\n&/g' | sed -n 's/^"trades":\([0-9]*\).*/\1/p' | awk '{s+=$1} END {print s+0}'; }

$DC -f $F up -d --build
i=0
until wget -qO- $URL/readyz >/dev/null 2>&1; do
	i=$((i + 1)); [ $i -lt 120 ] || fail "venue not ready in 120 s"; sleep 1
done
echo "  [l2book-box] ready"
sleep 3
st=$(wget -qO- $URL/stats)
echo "$st" | grep -q '"bid":-9223372036854775808' && fail "a symbol has no bid"
echo "$st" | grep -q '"ask":-9223372036854775808' && fail "a symbol has no ask"
echo "  [l2book-box] every book quoted"
t0=$(trades); sleep 10; t1=$(trades)
[ "$t1" -gt "$t0" ] || fail "no trades in 10 s ($t0 -> $t1)"
echo "  [l2book-box] trading: $t0 -> $t1 trades in 10 s"
wget -qO- $URL/ | grep -q '<title>l2book' || fail "landing page"
wget -qO- $URL/console.html | grep -q '<title>l2book console' || fail "console page"
wget -qO- $URL/console.js | grep -q 'con_reset' || fail "console.js"
wget -qO /dev/null $URL/style.css || fail "style.css"
wget -qO /dev/null $URL/book.wasm || fail "book.wasm"
wget -qO- $URL/symbols | grep -q '"AAPL"' || fail "/symbols (the console's LIVE probe)"
sse=$(timeout 3 wget -qO- "$URL/md?sym=AAPL" 2>/dev/null || true)
echo "$sse" | grep -q '^event: level' || fail "SSE: no level events"
echo "$sse" | grep -q '^event: trade' || fail "SSE: no trade events"
echo "  [l2book-box] landing page, console, book.wasm, /symbols and SSE market data served"
$DC -f $F restart venue
i=0
until wget -qO- $URL/readyz >/dev/null 2>&1; do
	i=$((i + 1)); [ $i -lt 60 ] || fail "not ready after restart"; sleep 1
done
sleep 3
wget -qO- $URL/stats | grep -q '"bid":-9223372036854775808' && fail "unquoted after restart"
echo "  [l2book-box] restart: ready and quoted again"
$DC -f $F down
echo "  [l2book-box] OK"
