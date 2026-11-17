# Heart & Soul — World Host (Windows)

**Minecraft-server model:** this PC is the single source of truth for the shared
world. Phones are thin clients — they send input/position/flag claims and
display host-streamed state. They never apply each other’s raw ciphertext.

Both phones still run the Heart & Soul ROM locally for rendering/input; partner
avatars and shared story traffic come only from this host.

MQTT is PIN signaling (+ encrypted frame fallback). Optional LAN TCP **27770**.
Does **not** modify `tools/HeartSoulCoopServer`.

## Quick start

1. Unzip the release (or build — see below).
2. Double-click `Start-WorldHost.bat` (or run `HeartSoulWorldHost.exe`).
3. Read the **ROOM PIN** in the console.
4. On each phone: Co-op → **Join PC Host (PIN)** → enter the PIN (no other prompts).
5. Leave the window open while you play.

PIN is stored in `%LOCALAPPDATA%\HeartSoulWorldHost\host.json`.  
Run with `--new-pin` to rotate it.

## What the host owns

- Each player's map / x / y / facing / follower (POSITION claims → host tables)
- Shared flags / vars / ready / config / ordered SIO (host-stamped stream)
- ~5 Hz re-stream of last positions so presence never expires and phones never invent tiles
- Presence resync after reconnect / field return
- Room keepalive (~90s grace on brief mobile drops)

Protocol **v3**: host encrypts all outbound game frames with sender id **2**.

## Network

- Outbound MQTT TLS to `broker.emqx.io:8883` (signaling)
- WebRTC ICE via built-in Metered STUN/TURN (no typing TURN URLs)
- Optional: TCP `27770` for same-LAN / port-forwarded play

## Build (developers)

```bat
dotnet publish HeartSoulWorldHost.csproj -c Release -r win-x64 --self-contained true
```
