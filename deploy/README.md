# OpenChat local dev / integration stack

A Docker Compose environment that runs the OpenChat relay behind a Caddy TLS
terminator with PostgreSQL, plus a dev CA the client can trust.

```
        HTTPS :443                  plain HTTP/WS (internal)         :5432
client ──────────────▶  caddy  ─────────────────────────▶  relay  ─────────▶  postgres
        (dev CA)      TLS term.       /v1/*  +  /v1/live           migrations
```

- **caddy** terminates TLS with a locally generated dev certificate and
  reverse-proxies the relay API (`/v1/*`) and the `/v1/live` WebSocket upgrade.
- **relay** is the ciphertext-only relay. It serves **plain HTTP/WS** on the
  private compose network only (never published to the host) and applies its
  schema migrations at startup. This preserves the relay's security model: TLS
  is terminated by the proxy in front of it (see `relay/src/RelayServer.cpp`).
- **postgres** is the backing store, on a named volume with a healthcheck.

Everything is **dev only**. The Postgres password and TLS certificate are
throwaway development material — never reuse them in production.

## Prerequisites

- Docker with the Compose plugin.
- OpenSSL (for the cert generator).

## 1. Generate the dev CA and server certificate

```sh
cd deploy/dev-ca
./gen-certs.sh
```

This writes the CA and server key/cert into `deploy/dev-ca/out/` (gitignored)
and prints the path to the root CA certificate. See `dev-ca/README.md` for
details. Regenerate anytime with `./gen-certs.sh --force`.

## 2. Bring the stack up

```sh
cd deploy
docker compose up -d --build
```

The relay image builds from `relay/Dockerfile`. On first run Compose also pulls
`postgres:16-alpine` and `caddy:2-alpine`.

Check status and logs:

```sh
docker compose ps
docker compose logs relay      # expect: "openchat-relay listening on 0.0.0.0:8443"
```

## 3. HTTPS endpoint

The stack publishes HTTPS on the host at:

```
https://localhost/v1/...
```

Example request (trusting the dev root CA):

```sh
curl --cacert deploy/dev-ca/out/rootCA.crt \
  -X POST https://localhost/v1/accounts \
  -H 'Content-Type: application/cbor' \
  --data-binary @<canonical-cbor-body>
```

The relay API speaks CBOR over `/v1/accounts`, `/v1/auth/*`, `/v1/sync`,
`/v1/key-packages*`, and a binary WebSocket at `/v1/live`.

## 4. Trusting the dev CA from the OpenChat client

The client defaults to the hosted relay at `https://chat.rigidstudios.de/v1`.
To connect to this local stack, launch from the repository root with:

```sh
OPENCHAT_RELAY_BASE_URL=https://localhost/v1 \
OPENCHAT_DEV_CA="$PWD/deploy/dev-ca/out/rootCA.crt" \
./Build/dev-release/OpenChat
```

Point the client's TLS trust store / CA-pin setting at:

```
deploy/dev-ca/out/rootCA.crt
```

The client still performs full TLS validation — there is no TLS bypass. It
simply trusts this additional dev CA so the mounted server certificate for
`localhost` / `relay.localhost` validates.

## 5. Tear down

```sh
docker compose down -v      # -v also removes the postgres volume
```

## The relay image

`relay/Dockerfile` is a two-stage build:

- **build stage** — `debian:trixie-slim`, which ships **Qt 6.8.2**, the exact
  version this project targets (the relay uses Qt 6.8 APIs such as
  `QHttpServerWebSocketUpgradeResponse`). It apt-installs Qt6
  (`qt6-base-dev`, `qt6-websockets-dev`, `qt6-httpserver-dev`), OpenSSL
  (`libssl-dev`) and libpq (`libpq-dev`). **Qt is never built from source.** It
  compiles only the `openchat-relay` target plus the two libraries it links
  against (`openchat_domain`, `openchat_protocol`), driven by the trimmed
  `deploy/relay-image/CMakeLists.txt` — the Qt Quick client, QtKeychain,
  SqlCipher and the Rust MLS library are not part of this image. The relay
  migrations are compiled into the binary as Qt resources.
- **runtime stage** — a slim `debian:trixie-slim` with just the relay binary and
  the Qt6 runtime libraries, including the PostgreSQL SQL driver plugin
  (`libqt6sql6-psql`, which pulls `libpq5`). It runs as an unprivileged user.

### Configuration (environment, per `relay/src/main.cpp`)

| Variable                       | Value in compose        |
| ------------------------------ | ----------------------- |
| `OPENCHAT_RELAY_PG_HOST`       | `postgres`              |
| `OPENCHAT_RELAY_PG_PORT`       | `5432`                  |
| `OPENCHAT_RELAY_PG_USER`       | `ocrelay`               |
| `OPENCHAT_RELAY_PG_PASSWORD`   | `devpassword` (dev only)|
| `OPENCHAT_RELAY_PG_DATABASE`   | `openchat_relay`        |
| `OPENCHAT_RELAY_BIND`          | `0.0.0.0` (internal net)|
| `OPENCHAT_RELAY_PORT`          | `8443`                  |

The relay applies migrations `001`–`003` itself at startup (see
`PostgresStore::applyMigrations`), so no separate migration job is needed.

## Verification status

This stack was brought up and verified end-to-end in a Docker-capable
environment:

- Relay image built from `relay/Dockerfile` with apt-provided Qt 6.8.2.
- `docker compose up -d` started postgres (healthy), relay and caddy.
- Relay connected to Postgres and applied all three migrations (12 tables
  created, `schema_migrations` = versions 1–3).
- **TLS → relay:** `POST https://localhost/v1/accounts` with an empty body
  returned the relay's `400`, proving Caddy terminates TLS and proxies to the
  live relay.
- **TLS → relay → Postgres:** a valid CBOR registration returned `200`, and the
  `accounts` and `devices` rows were then present in Postgres.
- **TLS is enforced:** the same request without `--cacert` failed certificate
  verification.
- **WebSocket proxy:** an `Upgrade: websocket` request to `/v1/live` returned
  `101 Switching Protocols` through Caddy (responses carried both
  `Server: Caddy` and `Server: openchat-relay/`), proving the upgrade is proxied
  to the relay.

## Security notes

- Private keys and certificates are generated locally by `dev-ca/gen-certs.sh`
  and are gitignored; they are never committed.
- The relay is never exposed to the host directly; only Caddy is published, and
  only on `:443`.
- These certificates and the Postgres password are for development only.
