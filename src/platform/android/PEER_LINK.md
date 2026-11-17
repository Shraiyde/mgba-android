# Heart & Soul peer link

**Primary path:** Windows **World Host** over **ZeroTier** (or Tailscale) — see `docs/coop-world-host.md`.

Phones: Co-op → **Join PC Host** → enter **PIN**. Host address defaults to
`10.137.178.161:27770` (remembered; advanced field editable). Status:
**Connected to PC host** / **Reconnecting…**.

Game data is **direct TCP** to the PC. No MQTT, no WebRTC, no TURN, no UPnP.
Host-stamped frames use AES-GCM sender id **2**.

Legacy opaque relay: `tools/HeartSoulCoopServer` (untouched).
