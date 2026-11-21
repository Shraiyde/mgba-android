# Heart & Soul peer link

**Primary path:** online **World Host** over public TCP — see `docs/coop-world-host.md`.

Phones: Co-op → **Join Online** (one tap). Host address, port, and shared auth
secret are baked into the APK. Status: **Connected to host** / **Reconnecting…**,
plus the persistent Host/Partner HUD.

Game data is **direct TCP** to the host. No MQTT, no WebRTC, no TURN, no UPnP, no PIN.
Host-stamped frames use AES-GCM sender id **2**.

Legacy opaque relay: `tools/HeartSoulCoopServer` (untouched).
