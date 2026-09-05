# OpenChat behind the host's Cloudflare Tunnel

Run from the repository root:

```sh
docker compose --env-file deploy/.env.tunnel -f deploy/compose.tunnel.yaml up -d --build
```

`deploy/.env.tunnel` must contain a generated `OPENCHAT_PG_PASSWORD`. Keep the
file private (mode 0600); it is ignored by Git. Do not change the password after
database initialization without also rotating the database role password.

The host Docker service must be enabled at boot. All three services use
`restart: unless-stopped`, so Docker restores them after reboot. Manually stopped
containers stay stopped until started again. PostgreSQL data lives in the
`openchat_pgdata` named volume; do not delete that volume during updates.

The host's existing, remotely managed cloudflared system service is reused.
In its Cloudflare dashboard tunnel, add a published application route:

- Hostname: `chat.rigidstudios.de`.
- Service type: HTTP.
- URL: localhost:18080.
- Leave the path filter empty so all API routes reach the proxy.

Only Caddy is published, on host loopback. The relay and PostgreSQL are on an
internal Docker network. Cloudflare provides public HTTPS; Caddy handles the
WebSocket subprotocol response and request limits. Do not put browser login or
interactive challenge rules in front of this native client's API. Bypass caching
for the chat hostname and leave WebSockets enabled.

The client defaults to `https://chat.rigidstudios.de/v1`; no environment variables
or development CA are needed. To use another deployment, set
`OPENCHAT_RELAY_BASE_URL=https://YOUR_HOSTNAME/v1` with no trailing slash.
For the local development stack, explicitly set
`OPENCHAT_RELAY_BASE_URL=https://localhost/v1` and `OPENCHAT_DEV_CA` to its root CA.

Check service status and logs:

```sh
docker compose --env-file deploy/.env.tunnel -f deploy/compose.tunnel.yaml ps
docker compose --env-file deploy/.env.tunnel -f deploy/compose.tunnel.yaml logs --tail=50 relay caddy
curl -i -X POST http://127.0.0.1:18080/v1/accounts
```

The empty account request should return 400. This checks routing without creating
an account. Perform the same check against the public HTTPS hostname after routing
it in Cloudflare. A WebSocket handshake must return status 101 and the subprotocol
`openchat.ciphertext.v1`.

Back up PostgreSQL regularly to a separate machine. This deployment does not
configure host/VLAN isolation or an automated backup destination.
