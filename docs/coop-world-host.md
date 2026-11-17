# Co-op World Host (Minecraft-server model)

Stephen’s framing: **the PC hosts the world; phones stream it.**

```
Phone A ──claims──┐
                  ├── HeartSoulWorldHost.exe  ← single source of truth
Phone B ──claims──┘         │
         ▲                  │ canonical frames (sender id 2)
         └──── stream ──────┘
         MQTT = PIN signaling (+ encrypted frame fallback)
```

## Roles

| Actor | Does | Must not |
| --- | --- | --- |
| **PC World Host** | Own positions, flags, vars, ready/config, ordered SIO; decrypt claims; re-encrypt and stream | Opaque-forward phone ciphertext |
| **Phone** | Run local ROM for display/input; send claims (POSITION / flags / SIO); apply **only** host-stamped frames | Guess peer motion; apply peer-to-peer ciphertext; invent world state |

## Phone UI

- **Join PC Host (PIN)** / Reconnect / Play Offline — PIN only
- Status: `PC Host (Direct)` or `PC Host (TURN)`

## Protocol (v3)

1. Phone encrypts a claim with its role sender id (0/1) → WebRTC/MQTT up.
2. Host decrypts with room session key `HMAC-SHA256(pinKey, session + "|HNS-WORLD-HOST-V1")`.
3. Host updates its world tables (per-player POSITION, etc.).
4. Host builds a fresh peer frame and encrypts with **sender id 2** + host counter.
5. Host delivers to the companion (WebRTC preferred, else LAN, else MQTT).
6. Phone **rejects** any frame whose sender ≠ 2 while in PC-host mode.
7. Host re-streams last positions ~10 Hz so presence never expires and phones never invent tiles.

## Crypto / transport

- Shared room AES key as above (also in `waiting` / `peer` control as Base64)
- WebRTC data channel + Metered TURN/STUN baked in
- Optional LAN TCP `:27770`

## Disconnects

- Host heartbeat ~8s; phone grace **90s**
- Soft partner drop keeps PC session (no re-PIN); WebRTC re-offer
- `presence-resync` + host POSITION snapshot on return

Legacy `tools/HeartSoulCoopServer` is untouched (opaque relay only).
