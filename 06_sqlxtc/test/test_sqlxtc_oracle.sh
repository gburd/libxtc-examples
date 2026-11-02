#!/bin/sh
# 06_sqlxtc/test/test_sqlxtc_oracle.sh
# Differential test: run a corpus of SQL through sqlxtc and
# compare results against python sqlite3 (which embeds SQLite
# directly).  Validates the Quack frontend doesn't lose data
# on the wire.

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"   # 06_sqlxtc/test
SERVER="${SQLXTC_SERVER:-$HERE/../sqlxtc-server}"

if [ ! -x "$SERVER" ]; then
	echo "  [sqlxtc-oracle] SKIP: $SERVER not built"
	exit 0
fi

if ! command -v python3 >/dev/null 2>&1; then
	echo "  [sqlxtc-oracle] SKIP: python3 not on PATH"
	exit 0
fi

if ! python3 -c "import sqlite3" 2>/dev/null; then
	echo "  [sqlxtc-oracle] SKIP: python3 has no sqlite3 module"
	exit 0
fi

cd "$HERE"
python3 "$HERE/test_sqlxtc_oracle.py" 2>&1 | \
	grep -E "ok|FAIL|Results:" | tail -10
exit ${PIPESTATUS[0]:-0}
