# syntax=docker/dockerfile:1

# OpenChat relay image (build context = repository root).
#
# Two stages:
#   1) build   - apt-provided Qt6 6.8 (the version this project targets) plus
#                OpenSSL and libpq; compiles ONLY the openchat-relay target and
#                the two libraries it links against. No Qt built from source.
#   2) runtime - slim Debian carrying just the relay binary and the Qt6 runtime
#                libraries, including the QPSQL SQL driver plugin (libpq).
#
# TLS is intentionally NOT handled in this image: the relay serves plain HTTP/WS
# on the private compose network and Caddy terminates TLS in front of it, which
# matches the relay's security model (see relay/src/RelayServer.cpp).

# ---------------------------------------------------------------------------
# Stage 1: build
# ---------------------------------------------------------------------------
FROM debian:trixie-slim AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        make \
        g++ \
        qt6-base-dev \
        qt6-base-dev-tools \
        qt6-websockets-dev \
        qt6-httpserver-dev \
        libssl-dev \
        libpq-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Minimal source surface for the relay: the relay module plus the domain and
# protocol libraries it links against. Nothing from the Qt Quick client is
# copied in, keeping the build context and image small.
COPY relay/ relay/
COPY src/core/ src/core/
COPY src/domain/ src/domain/
COPY src/protocol/ src/protocol/
COPY deploy/relay-image/CMakeLists.txt CMakeLists.txt

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target openchat-relay -j"$(nproc)"

# ---------------------------------------------------------------------------
# Stage 2: runtime
# ---------------------------------------------------------------------------
FROM debian:trixie-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libqt6core6t64 \
        libqt6network6 \
        libqt6sql6 \
        libqt6sql6-psql \
        libqt6websockets6 \
        libqt6httpserver6 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Run as an unprivileged account; the relay binds a high port (>1024).
RUN useradd --system --no-create-home --uid 10001 relay

COPY --from=build /src/build/relay/openchat-relay /usr/local/bin/openchat-relay

USER relay

# Plain HTTP/WS on the internal network; TLS terminates at the Caddy proxy.
# Postgres connection settings are supplied by compose via the environment.
ENV OPENCHAT_RELAY_BIND=0.0.0.0 \
    OPENCHAT_RELAY_PORT=8443

EXPOSE 8443
ENTRYPOINT ["/usr/local/bin/openchat-relay"]
