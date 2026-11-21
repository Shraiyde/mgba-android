# Heart & Soul — World Host

**Online Minecraft-server model.** Phones dial a public TCP host (Fly.io) with
credentials **baked into the APK and this host** — no PIN, no address typing.

No MQTT, no WebRTC, no TURN, no UPnP, no ZeroTier required on phones.

## Quick start (phones / Petra)

1. Install the latest co-op APK.
2. Heart & Soul → Co-op → **Join Online** (one tap).
3. Wait a few seconds if the host was idle (cold start ~2–3 s).

Status on phones: `Connected to host` / `Reconnecting…`, plus the persistent HUD.

## Optional — Windows host (offline / LAN)

Still useful if you want a local host instead of the online one (requires matching
baked address in that build):

1. Unzip the release and run `Start-WorldHost.bat`.
2. Approve the **Windows Firewall** UAC prompt once if asked.

## Fly.io (Stephen)

- App: `heartsoul-host` (`fly.toml`), region `fra`, raw TCP `27770`.
- Build/run via `Dockerfile`. Redeploy from Stephen’s side — agents do not deploy.

## Build

```bat
dotnet publish HeartSoulWorldHost.csproj -c Release -r win-x64 --self-contained true
```
