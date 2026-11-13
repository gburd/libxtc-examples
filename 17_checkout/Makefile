# Build libxtc first.  The root `make check` also builds and runs this as
# 17_checkout (check-examples).
XTC_BUILD ?= ../../build_unix
XTC_INC ?= ../../src/inc
CC = cc
CFLAGS ?= -O2 -g
CPPFLAGS += -I$(XTC_INC) -I$(XTC_BUILD)
XTC_LDFLAGS := $(shell sed -n 's/^LDFLAGS[[:space:]]*=[[:space:]]*//p' $(XTC_BUILD)/Makefile)
XTC_LIBS := $(shell sed -n 's/^LIBS[[:space:]]*=[[:space:]]*//p' $(XTC_BUILD)/Makefile)

.PHONY: all test clean
all: checkout
checkout: checkout.c $(XTC_BUILD)/libxtc.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic -o $@ \
	    checkout.c $(XTC_BUILD)/libxtc.a $(LDFLAGS) $(XTC_LDFLAGS) \
	    $(XTC_LIBS) -pthread

test: checkout
	./checkout > checkout.out
	diff -u expected.txt checkout.out
	./checkout --help >/dev/null
	@rm -f checkout.out

clean:
	rm -f checkout checkout.out
