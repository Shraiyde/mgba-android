# Heart & Soul — World Host (Windows)

Authoritative co-op host for two Android phones. **Your PC** runs this app;
both phones join with the 6-digit PIN. Phones connect to the PC over **WebRTC
data channels** (built-in Metered TURN/STUN). MQTT is used only for
PIN pairing / signaling. Optional LAN TCP port **27770**.

This does **not** replace or modify `tools/HeartSoulCoopServer`.

## Quick start

1. Unzip the release (or build — see below).
2. Double-click `Start-WorldHost.bat` (or run `HeartSoulWorldHost.exe`).
3. Read the **ROOM PIN** in the console.
4. On each phone: Co-op → **Join PC Host (PIN)** → enter the PIN.
5. Leave the window open while you play. Status lines show each phone and
   whether they are on WebRTC / LAN / signaling.

PIN is stored in `%LOCALAPPDATA%\HeartSoulWorldHost\host.json`.  
Run with `--new-pin` to rotate it.

## What the host owns

- Each player's latest map / x / y / facing / follower (from POSITION frames)
- Ordered frame relay phone↔phone (WebRTC preferred, MQTT/LAN fallback)
- Presence resync after reconnect / field return
- Room keepalive (does not tear down on brief mobile drops; ~90s grace)

Both phones still run the Heart & Soul ROM locally for gameplay/rendering.
Partner avatars and shared story traffic are coordinated through this host.

## Network

- Outbound MQTT TLS to `broker.emqx.io:8883` (signaling only)
- WebRTC ICE via built-in Metered STUN/TURN (no typing TURN URLs)
- Optional: TCP `27770` for same-LAN / port-forwarded play

No cloud game hosting and no paid subscription are required.

## Build (developers)

```bat
dotnet publish tools\HeartSoulWorldHost\HeartSoulWorldHost.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -o publish-world-host
```
