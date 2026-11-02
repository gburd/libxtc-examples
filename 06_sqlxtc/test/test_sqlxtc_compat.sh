#!/bin/sh
# 06_sqlxtc/test/test_sqlxtc_compat.sh
# SQL-dialect compatibility differential test: run a self-contained
# corpus through sqlxtc (via Quack) and python sqlite3, asserting
# identical result sets on the supported-feature set and reporting
# (without failing on) the documented unsupported-feature gaps.
#
# This is NOT a run of SQLite's testfixture TCL suite -- sqlxtc is a
# from-scratch, wire-protocol engine, not SQLite's C library, so that
# suite does not apply.  The larger against-~/src/sqlite measurement
# is the opt-in harvest_sqlite_tests.py (not part of make check).

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"   # 06_sqlxtc/test
SERVER="${SQLXTC_SERVER:-$HERE/../sqlxtc-server}"

if [ ! -x "$SERVER" ]; then
	echo "  [sqlxtc-compat] SKIP: $SERVER not built"
	exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "  [sqlxtc-compat] SKIP: python3 not on PATH"
	exit 0
fi
if ! python3 -c "import sqlite3" 2>/dev/null; then
	echo "  [sqlxtc-compat] SKIP: python3 has no sqlite3 module"
	exit 0
fi

cd "$HERE"
python3 "$HERE/test_sqlxtc_compat.py" 2>&1 | \
	grep -E "FAIL|XBUG|xpass|CORPUS|XFAIL|compat" | tail -20
exit ${PIPESTATUS[0]:-0}
