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
WASM_LDFLAGS = -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined \
               -Wl,--initial-memory=33554432

.PHONY: all check bench wasm clean

all: test_book bench_book

test_book: book.c book.h refbook.h test_book.c
	$(CC) $(CFLAGS) -o $@ book.c test_book.c

bench_book: book.c book.h hist.h bench_book.c
	$(CC) $(CFLAGS) -o $@ book.c bench_book.c

check: test_book
	./test_book

bench: bench_book
	taskset -c 2 ./bench_book 2>/dev/null || ./bench_book

clean:
	rm -f test_book bench_book
