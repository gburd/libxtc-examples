# 16_l2book/Dockerfile -- the venue and its console in one image.
#
# Build context: a libxtc checkout whose examples/ is this repository
# (the submodule), so libxtc and the example build from the same tree:
#
#   docker compose -f examples/16_l2book/compose.yaml up --build
#
# Stage 1 builds libxtc (epoll backend: a container may not be allowed
# io_uring), the venue, and book.wasm with clang's wasm32 target.
FROM debian:trixie-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc make autoconf automake libtool pkg-config clang lld \
        ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cd dist && autoreconf -i >/dev/null 2>&1 || true
RUN mkdir build && cd build && \
    ../dist/configure --with-io-backend=epoll --with-tls=none \
        --without-liburing >/dev/null && \
    make -j"$(nproc)" libxtc.a >/dev/null
RUN cd examples/16_l2book && \
    make XTC_BUILD=../../build XTC_INC=../../src/inc l2book l2book-flow test_book && \
    make wasm WASM_CC=clang && \
    ./test_book >/dev/null && ./l2book --self-test

# Stage 2: the binary and the console, nothing else.
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends wget \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/examples/16_l2book/l2book /usr/local/bin/l2book
COPY --from=build /src/examples/16_l2book/l2book-flow /usr/local/bin/l2book-flow
COPY --from=build /src/examples/16_l2book/web /srv/l2book/web
WORKDIR /srv/l2book
EXPOSE 8080 9000 9001
ENTRYPOINT ["l2book"]
CMD ["--flow", "--web-root", "/srv/l2book/web"]
