#!/bin/sh
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# 05_rexis/test/test_rexis_persist.sh
#   Run the rexis db_t persistence test (Bitcask-backed SET/GET/DEL
#   recovery across simulated restart).  This wraps a self-contained
#   C binary; it does NOT background the network server, which is
#   why earlier shell-only versions of this test could hang.

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"   # 05_rexis/test
BIN="${REXIS_PERSIST_BIN:-$HERE/../test_db_persist}"

if [ ! -x "$BIN" ]; then
	echo "  [rexis-persist] SKIP: $BIN not built"
	exit 0
fi

"$BIN"
