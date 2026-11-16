# 20_jobq/Makefile -- the job queue and its deterministic simulation test.
#
# Targets:
#   make              jobq (real threads) and jobq_dst (the DST harness)
#   make check        jobq once, then 300 seeds of jobq_dst (about 1 s)
#   make sweep        SEEDS seeds (default 20000) from BASE (default 0)
#   make check-plant  each planted bug, which the harness must catch
#   make clean
#
# jobq builds against a native libxtc (XTC_BUILD).  jobq_dst must link
# a simulation build: a libxtc configured --with-io-backend=sim
# (XTC_SIM_BUILD, which defaults to XTC_BUILD).  Linked against a native
# build, jobq_dst says "not a sim build" and exits 0.
#
# PLANT=n builds bug n into jobq_dst (1 no fence, 2 ack before durable,
# 3 a restarted queue reuses its epoch).  The defaults work inside a
# libxtc checkout (examples/ is the submodule).

XTC_BUILD     ?= ../../build_unix
XTC_SIM_BUILD ?= $(XTC_BUILD)
XTC_INC       ?= ../../src/inc

CC     ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

# libxtc's own link flags (liburing, TLS, ...) come from its Makefile.
build_libs = $(shell [ -f $(1)/Makefile ] && \
    sed -n 's/^LIBS[[:space:]]*=[[:space:]]*//p' $(1)/Makefile)
LIBXTC     = $(XTC_BUILD)/libxtc.a -pthread -ldl -lm \
             $(call build_libs,$(XTC_BUILD))
LIBXTC_SIM = $(XTC_SIM_BUILD)/libxtc.a -pthread -ldl -lm \
             $(call build_libs,$(XTC_SIM_BUILD))

SEEDS ?= 20000
BASE  ?= 0
PLANTS = 1 2 3

.PHONY: all check sweep check-plant clean

all: jobq jobq_dst

jobq: main.c jobq.c jobq.h $(XTC_BUILD)/libxtc.a
	$(CC) $(CFLAGS) -I$(XTC_INC) -I$(XTC_BUILD) -o $@ main.c jobq.c \
	    $(LDFLAGS) $(LIBXTC)

jobq_dst: dst.c jobq.c jobq.h $(XTC_SIM_BUILD)/libxtc.a
	$(CC) $(CFLAGS) $(if $(PLANT),-DPLANT=$(PLANT)) -I$(XTC_INC) \
	    -I$(XTC_SIM_BUILD) -o $@ dst.c jobq.c $(LDFLAGS) $(LIBXTC_SIM)

check: jobq jobq_dst
	./jobq
	./jobq_dst 0 300
	./jobq --help >/dev/null
	./jobq_dst --help >/dev/null

sweep: jobq_dst
	./jobq_dst $(BASE) $(SEEDS)

# Each planted bug must make the 300-seed run fail and name the
# invariant it breaks.  Builds each into its own binary.
check-plant:
	@for p in $(PLANTS); do \
		$(CC) $(CFLAGS) -DPLANT=$$p -I$(XTC_INC) -I$(XTC_SIM_BUILD) \
		    -o jobq_dst_plant$$p dst.c jobq.c $(LDFLAGS) \
		    $(LIBXTC_SIM) || exit 1; \
		out=$$(./jobq_dst_plant$$p 0 300); \
		case "$$out" in \
		*"not a sim build"*) echo "check-plant: needs a sim build"; \
			exit 1 ;; \
		*FAIL*) printf 'plant %s caught: %s\n' $$p \
			"$$(printf '%s\n' "$$out" | head -1)" ;; \
		*) echo "plant $$p NOT caught in 300 seeds"; exit 1 ;; \
		esac; \
	done; rm -f jobq_dst_plant*

clean:
	rm -f jobq jobq_dst jobq_dst_plant*
