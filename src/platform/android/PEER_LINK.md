# Heart & Soul peer link

**Primary path:** Windows **World Host** (`tools/HeartSoulWorldHost`) over **ZeroTier** — see `docs/coop-world-host.md`.

Phones: Co-op → **Join PC Host** (one tap). Status: **Connected to PC host** / **Reconnecting…**.

Game data is **direct TCP** to the baked-in host. No MQTT, no WebRTC, no TURN, no PIN entry. Host-stamped frames use AES-GCM sender id **2**.

Legacy opaque relay: `tools/HeartSoulCoopServer` (untouched).
