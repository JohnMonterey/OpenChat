# UDP voice media (relay-assisted, WS fallback)

OpenChat calls default to relay-assisted UDP for voice packets, eliminating TCP
head-of-line blocking while preserving end-to-end encryption and NAT traversal.
Signaling, call setup, MLS group management, camera video (v2), and screen shares
(v3) continue over the authenticated WebSocket connection.

---

## Wire Protocol & Frame Format

All UDP frames begin with a 1-byte message type discriminator followed by
type-specific fields:

| Type Byte | Message | Fields |
| :--- | :--- | :--- |
| `0x01` | **Bind** | `token[16]` |
| `0x02` | **BindOk** | *(empty)* |
| `0x03` | **Forward** | `callId[16]` \| `fromDeviceId[16]` \| `payload[N]` |
| `0x04` | **Heartbeat** | `sequence[4]` (big-endian `uint32_t`) |
| `0x05` | **HeartbeatAck**| `sequence[4]` (big-endian `uint32_t`) |
| `0x06` | **Bye** | `callId[16]` |

### Media Tokens
1. When a call enters `Active` or media starts, the client sends control frame `[12]`
   over its authenticated WebSocket to the relay.
2. The relay generates a single-use 16-byte cryptographically secure token, records
   the associated `(account, device)` pairing with a 60-second TTL, and replies with
   control frame `[13, token]`.
3. The client sends a `Bind` frame containing this token to the relay's UDP media port.
4. The relay validates the token, maps the sender's UDP endpoint `(IP, port)` to the
   device, invalidates the token, and returns `BindOk`.

---

## Voice Only

Only Opus voice packets (version `1`, payload $\le$ 1400 bytes) travel over UDP.
Large frames—such as camera video (version `2`, up to 96 KiB) and screen shares
(version `3`, up to 128 KiB)—remain on the WebSocket datagram path. Packets exceeding
1400 bytes presented to the UDP path are dropped to prevent UDP fragmentation.

---

## Failover & Recovery

The client manages connection state via `UdpCallMediaPath`:

```
   [ Disabled / Init ]
           │
           ▼
       [ Probing ] ──(BindOk + HeartbeatAck)──► [ Active ]
           ▲                                        │
           │                                 (3 missed pings
           │                                  or explicit loss)
           │                                        │
           └────────(retry every 5 s)────────── [ Fallback ]
                                             (Audio sent via WS)
```

- **Probing**: The client sends `Bind` with the acquired token and begins sending
  periodic `Heartbeat` pings every 500 ms.
- **Active**: Once `BindOk` and at least one `HeartbeatAck` arrive, voice frames are
  diverted to UDP `Forward` frames. The UI displays `UDP · <RTT> ms`.
- **Fallback**: If 3 consecutive heartbeats go unacknowledged (1500 ms) or UDP socket
  errors occur, the transport silently falls back to WebSocket datagrams without
  tearing down the call. The UI displays `Relay (TCP)`.
- **Re-probing**: While in fallback, the client periodically attempts UDP binding every
  5 seconds to recover when network paths restore.
- **Bye**: Hanging up or tearing down member media transmits a `Bye` UDP frame to
  promptly clean up relay binding state. Disconnecting the WebSocket also clears
  all UDP bindings for that device.

### Transport Settings
The **Audio & Video → Connection** settings panel provides three user modes:
- **Auto (UDP with Relay fallback)** *(default)*: Uses UDP voice media whenever
  possible, transparently falling back to WebSocket if blocked.
- **UDP Only**: Requires UDP voice; suppresses WebSocket voice fallback to diagnose
  packet routing.
- **Relay (TCP) Only**: Forces all media to ride WebSocket datagrams over TLS.

---

## Deployment & Network Configuration

The relay exposes a single UDP port for voice media alongside its HTTP/WS port:

- Default UDP port: `8444/udp`
- Configurable environment variables:
  - `OPENCHAT_RELAY_MEDIA_BIND`: bind interface (default `0.0.0.0`).
  - `OPENCHAT_RELAY_MEDIA_PORT`: UDP listening port (default `8444`).

### Cloudflare Tunnel & Reverse Proxies
Standard reverse proxies (Caddy, Nginx) and Cloudflare Tunnels (`cloudflared`) terminate
HTTP/TLS and WebSocket traffic, but **do not forward raw UDP datagrams**.

When deploying behind Cloudflare or reverse proxies:
- Port `8444/udp` must be published and port-forwarded directly on the host or firewall,
  or routed through a UDP-capable reverse proxy / Cloudflare Spectrum.
- If UDP is blocked by a network proxy, clients automatically and transparently fall
  back to WebSocket datagrams over the existing HTTPS/TLS connection.

---

## Threat Model & Security

- **E2E Ciphertext Preservation**: The relay receives and forwards opaque `payload`
  bytes. The media encryption layer (ChaCha20-Poly1305 with per-call HKDF keys,
  replay windows, and sequence authentication) is unchanged.
- **No Spoofing**: UDP bindings require a fresh, single-use 16-byte token issued over
  the authenticated, encrypted WebSocket connection.
- **IP Re-pinning**: If a client's public NAT mapping or cell/Wi-Fi connection shifts,
  incoming `Forward` or `Heartbeat` frames with the matching device ID and call ID
  update the endpoint map.
- **Resource Limits**: Stale bindings expire after 60 seconds of inactivity. Tokens
  expire after 60 seconds if unused.
