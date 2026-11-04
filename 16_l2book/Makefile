# 16_l2book/Makefile -- the l2book limit order book example.
#
# Targets:
#   make              the engine tests, the engine benchmark and the venue
#   make check        run test_book (unit + differential + determinism),
#                     the venue's self-test, and the --help lint
#   make bench        the bare-engine benchmark (pinned to one core)
#   make wasm         web/book.wasm (the engine for the browser console)
#   make clean
#
# XTC_BUILD / XTC_INC point at a libxtc build tree and its headers; the
# defaults work inside a libxtc checkout (examples/ is the submodule).

XTC_BUILD ?= ../../build_unix
XTC_INC   ?= ../../src/inc

CC      ?= cc
CFLAGS  ?= -O3 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -D_DEFAULT_SOURCE -I. -I$(XTC_INC)

# libxtc's own link flags (liburing, TLS, ...) come from its Makefile.
XTC_LIBS := $(shell [ -f $(XTC_BUILD)/Makefile ] && \
    awk '/^LIBS[[:space:]]*=/{sub(/^LIBS[[:space:]]*=[[:space:]]*/,""); print}' \
    $(XTC_BUILD)/Makefile 2>/dev/null)
XTC_LDFLAGS := $(shell [ -f $(XTC_BUILD)/Makefile ] && \
    awk '/^LDFLAGS[[:space:]]*=/{sub(/^LDFLAGS[[:space:]]*=[[:space:]]*/,""); print}' \
    $(XTC_BUILD)/Makefile 2>/dev/null)
XTC_CINC := $(shell [ -f $(XTC_BUILD)/Makefile ] && \
    awk '/^CFLAGS[[:space:]]*=/{print}' $(XTC_BUILD)/Makefile 2>/dev/null | \
    grep -o '\-I[^ ]*' | grep -v 'src/inc' | tr '\n' ' ')
LIBXTC = $(XTC_LDFLAGS) $(XTC_BUILD)/libxtc.a -pthread -ldl -lm $(XTC_LIBS)

# A freestanding wasm32 clang (no libc); WASM_CC=... to override.
WASM_CC     ?= clang
WASM_FLAGS   = --target=wasm32 -nostdlib -ffreestanding -O3 -std=c11 \
               -mbulk-memory -Wall -Wextra -I.
WASM_LDFLAGS = -Wl,--no-entry -Wl,--export-dynamic \
               -Wl,--initial-memory=33554432

.PHONY: all check bench wasm clean

VENUE_SRC = book.c venue.c fibers.c main.c
VENUE_HDR = book.h venue.h hist.h

all: test_book bench_book l2book l2book-flow

l2book-flow: flow.c
	$(CC) $(CFLAGS) -o $@ flow.c

l2book: $(VENUE_SRC) $(VENUE_HDR) $(XTC_BUILD)/libxtc.a
	$(CC) $(CFLAGS) $(XTC_CINC) -o $@ $(VENUE_SRC) $(LIBXTC)

test_book: book.c book.h refbook.h test_book.c
	$(CC) $(CFLAGS) -o $@ book.c test_book.c

bench_book: book.c book.h hist.h bench_book.c
	$(CC) $(CFLAGS) -o $@ book.c bench_book.c

# The browser engine: book.c + wasm.c, freestanding wasm32.  WASM_CC must
# be a clang with the wasm32 target and wasm-ld on PATH.
wasm: web/book.wasm

web/book.wasm: book.c book.h wasm.c
	$(WASM_CC) $(WASM_FLAGS) $(WASM_LDFLAGS) -o $@ book.c wasm.c

check: test_book l2book l2book-flow
	./test_book
	./l2book --self-test
	./l2book --help >/dev/null
	./l2book-flow --help >/dev/null

bench: bench_book
	taskset -c 2 ./bench_book 2>/dev/null || ./bench_book

clean:
	rm -f test_book bench_book l2book l2book-flow
