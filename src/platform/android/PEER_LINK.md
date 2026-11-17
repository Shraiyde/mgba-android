# Heart & Soul peer link

**Primary path:** Windows **World Host** over **ZeroTier** — see `docs/coop-world-host.md`.

Phones: Co-op → **Join PC Host** (one tap). Host address, port, and shared auth
secret are baked into the APK. Status: **Connected to PC host** / **Reconnecting…**.

Game data is **direct TCP** to the PC. No MQTT, no WebRTC, no TURN, no UPnP, no PIN.
Host-stamped frames use AES-GCM sender id **2**.

Legacy opaque relay: `tools/HeartSoulCoopServer` (untouched).
