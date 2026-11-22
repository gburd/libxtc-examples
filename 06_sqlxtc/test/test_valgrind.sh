#!/usr/bin/env bash
#-
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# examples/06_sqlxtc/test/test_valgrind.sh -- run every C test of
# `make test` once under valgrind memcheck (make test-valgrind).
#
# Usage (from 06_sqlxtc):
#   make XTC_BUILD=<libxtc build dir> test-valgrind
#
# The libxtc build must use a readiness I/O backend (configure
# --with-io-backend=epoll), as libxtc's own memcheck job does.  valgrind
# does not model io_uring: a page or socket buffer the kernel fills
# through a ring stays "undefined" to memcheck, so an io_uring build
# reports tens of thousands of false uninitialised-value errors (every
# one from the buffer pool), and its socket tests can hang.  The script
# refuses such a build.  Configure finding <valgrind/valgrind.h>
# (XTC_HAVE_VALGRIND_H) registers fiber stacks with valgrind; without it
# every fiber switch is reported as well.
#
# Each test runs with XTC_UNDER_VALGRIND=1, which relaxes the tests'
# timing and volume bounds (never a correctness check), and with
# --fair-sched=yes, without which a test whose threads spin on each
# other can run for hours.  The test list is `make -n test`'s, so a new
# test is covered once `make test` runs it; the Python and shell tests
# are skipped.  test-latched's loop is read from LATCHED_TESTS.  A test fails on any memcheck error or a nonzero exit.
#
# Environment: VG_JOBS (parallel tests, default nproc / 3), VG_TIMEOUT
# (seconds per test, default 3600), VG_OUT (logs, default a new
# directory under TMPDIR).
set -euo pipefail

build=${XTC_BUILD:?set XTC_BUILD to the libxtc build directory}
cfg="$build/xtc_config.h"
if [ ! -f "$cfg" ]; then
	echo "test_valgrind: no $cfg; is XTC_BUILD a libxtc build?" >&2
	exit 2
fi
if grep -q '^#define XTC_IO_BACKEND_URING 1' "$cfg"; then
	echo "test_valgrind: $build uses io_uring, which valgrind does not" \
		"model; configure a build with --with-io-backend=epoll" >&2
	exit 2
fi
if ! grep -q '^#define XTC_HAVE_VALGRIND_H 1' "$cfg"; then
	echo "test_valgrind: $build has no <valgrind/valgrind.h>: fiber" \
		"stacks are not registered" >&2
	exit 2
fi
command -v valgrind >/dev/null || {
	echo "test_valgrind: no valgrind" >&2
	exit 2
}

# The test commands of `make test`: a binary and its arguments, without
# the TMPDIR= prefix (each run gets its own TMPDIR below).
mapfile -t cmds < <(make -s -n XTC_BUILD="$build" ${XTC_INC:+XTC_INC="$XTC_INC"} test |
	sed -n -E 's/^(TMPDIR=[^ ]+ )?((\.|test)\/test_[a-z0-9_]+( [^;|&]*)?)$/\2/p' |
	sed -E 's/ +$//' | awk '!seen[$0]++')
# test-latched runs its binaries in a shell loop, which make -n prints
# unexpanded: take them from the Makefile's LATCHED_TESTS.
mapfile -t -O "${#cmds[@]}" cmds < <(sed -n 's/^LATCHED_TESTS = //p' Makefile |
	tr ' ' '\n' | sed -n 's|^\(test_[a-z0-9_]*\)$|./\1_latched|p')
if [ "${#cmds[@]}" -eq 0 ]; then
	echo "test_valgrind: found no tests in make -n test" >&2
	exit 2
fi
mapfile -t bins < <(printf '%s\n' "${cmds[@]}" | awk '{print $1}' |
	sed 's|^\./||' | sort -u)
make -s XTC_BUILD="$build" ${XTC_INC:+XTC_INC="$XTC_INC"} "${bins[@]}"

out=${VG_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/sqlxtc-vg.XXXXXX")}
mkdir -p "$out"
jobs=${VG_JOBS:-$(($(nproc) / 3 > 0 ? $(nproc) / 3 : 1))}
export VG_OUT_DIR="$out" VG_TIMEOUT="${VG_TIMEOUT:-3600}"

# One test under memcheck: prints "<name> ok|FAIL rc=N errors=N secs=N".
run_one() {
	local cmd=$1 name t rc s errs
	name=$(printf '%s' "$cmd" | tr ' /' '__' | sed 's/^\._//')
	t=$(mktemp -d "$VG_OUT_DIR/tmp.XXXXXX")
	s=$(date +%s)
	rc=0
	# shellcheck disable=SC2086 # $cmd is a binary and its arguments
	TMPDIR="$t" XTC_UNDER_VALGRIND=1 timeout "$VG_TIMEOUT" \
		valgrind --fair-sched=yes --error-limit=no --track-origins=yes \
		--num-callers=30 \
		--log-file="$VG_OUT_DIR/$name.%p.vg" $cmd \
		>"$VG_OUT_DIR/$name.out" 2>&1 || rc=$?
	errs=$(cat "$VG_OUT_DIR/$name".*.vg 2>/dev/null |
		sed -n 's/.*ERROR SUMMARY: \([0-9]*\).*/\1/p' |
		awk '{n += $1} END {print n + 0}')
	find "$t" -mindepth 1 -delete 2>/dev/null || true
	rmdir "$t" 2>/dev/null || true
	if [ "$rc" -eq 0 ] && [ "$errs" -eq 0 ]; then
		echo "$name ok rc=$rc errors=$errs secs=$(($(date +%s) - s))"
	else
		echo "$name FAIL rc=$rc errors=$errs secs=$(($(date +%s) - s))"
	fi
}
export -f run_one

echo "test_valgrind: ${#cmds[@]} tests, $jobs at a time, logs in $out"
# shellcheck disable=SC2016 # $1 expands in the child bash
printf '%s\n' "${cmds[@]}" |
	xargs -P "$jobs" -d '\n' -I{} bash -c 'run_one "$1"' _ {} |
	tee "$out/summary.txt"
nfail=$(grep -c ' FAIL ' "$out/summary.txt" || true)
echo "test_valgrind: $((${#cmds[@]} - nfail)) of ${#cmds[@]} clean, logs in $out"
[ "$nfail" -eq 0 ]
