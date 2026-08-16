# Dev CA and server certificate

`gen-certs.sh` produces a throwaway root CA and a server certificate for the
local OpenChat stack. Caddy uses the server certificate to terminate TLS; a
client (or `curl`) trusts the root CA to reach the stack over HTTPS.

**Dev only.** These keys and certificates are for local development and
integration testing. Never use them in production. All key material is written
under `out/`, which is gitignored and never committed.

## Generate

```sh
cd deploy/dev-ca
./gen-certs.sh          # idempotent: reuses existing material
./gen-certs.sh --force  # regenerate from scratch
```

## Output (`deploy/dev-ca/out/`, gitignored)

| File          | Purpose                                                        |
| ------------- | ------------------------------------------------------------- |
| `rootCA.crt`  | Dev root CA certificate — **trust/pin this** in the client.   |
| `rootCA.key`  | Root CA private key (signs the server cert). Never leaves here.|
| `server.crt`  | Server certificate mounted into Caddy.                        |
| `server.key`  | Server private key mounted into Caddy.                        |

The server certificate carries Subject Alternative Names `localhost`,
`relay.localhost`, `caddy`, `127.0.0.1` and `::1`, so it validates for the
HTTPS endpoint the stack publishes on the host (`https://localhost`) and for the
`caddy`/`relay.localhost` names on the compose network.

## Trusting the root CA

- **curl:** `curl --cacert deploy/dev-ca/out/rootCA.crt https://localhost/...`
- **OpenChat client:** point the client's trust store / CA-pin setting at
  `deploy/dev-ca/out/rootCA.crt`. The client always validates TLS — there is no
  TLS bypass; it simply trusts this additional dev CA.
- **System-wide (optional, Linux):** copy `rootCA.crt` into
  `/usr/local/share/ca-certificates/` and run `update-ca-certificates`.

## Security notes

- Private keys are generated locally and are gitignored; they are never
  committed to the repository.
- The CA is unconstrained and long-lived purely for local convenience. Treat it
  as compromised the moment it leaves your machine, and never install it on a
  system you also use for real browsing.
