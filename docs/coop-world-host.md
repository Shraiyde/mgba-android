# Co-op World Host architecture

## Model

Stephen’s **Windows PC** runs `tools/HeartSoulWorldHost`. Both phones are
clients. The host is authoritative for co-op sync state (POSITION /
follower / frame ordering / presence resync). Phones still run the ROM for
local gameplay and render the partner from host-forwarded state.

```
Phone A ──WebRTC──┐
                  ├── HeartSoulWorldHost.exe (Windows)
Phone B ──WebRTC──┘
         MQTT broker = PIN signaling only
```

## Phone UI

- **Join PC Host (PIN)** / Reconnect / Play Offline / Debug logs
- Status: `PC Host (Direct)` or `PC Host (TURN)`
- No server URL / TURN typing

## Transports

1. WebRTC data channel to the PC (Metered STUN/TURN built in)
2. MQTT frame fallback while ICE is up
3. Optional LAN TCP `:27770`

## Crypto

Shared room key: `HMAC-SHA256(pinKey, session + "|HNS-WORLD-HOST-V1")`
(also delivered in `waiting`/`peer` control as Base64). Host can decrypt
POSITION for authority while still forwarding opaque frames.

## Disconnects

- Host heartbeat every ~8s; phone watchdog grace **90s**
- Slots held across brief drops; WebRTC re-offer on Direct retry
- `presence-resync` control forces POSITION republish after return

Legacy `tools/HeartSoulCoopServer` is untouched (opaque relay only).
