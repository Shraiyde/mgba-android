# Heart & Soul — World Host (Windows)

**ZeroTier Minecraft-server model.** The PC listens for **plain TCP** from both
phones on **all interfaces**. Credentials (host address, port, shared auth
secret) are **baked into this EXE and the Android APK** — phones type nothing.

No MQTT, no WebRTC, no TURN, no UPnP, no PIN entry.

## Quick start (Stephen)

1. Leave **ZeroTier** connected (expect `10.137.178.161`).
2. Unzip the release and run `Start-WorldHost.bat`.
3. Approve the **Windows Firewall** UAC prompt once if asked.
4. On each phone: Co-op → **Join PC Host** (one tap).

Status on phones: `Connected to PC host` or `Reconnecting…`.

## Petra — phone ZeroTier setup

1. Install **ZeroTier One** (Play Store).
2. **Join Network** using Stephen’s ZeroTier network ID.
3. Keep ZeroTier on while playing.
4. Heart & Soul → Co-op → **Join PC Host** (one tap — no PIN, no address).

## Build

```bat
dotnet publish HeartSoulWorldHost.csproj -c Release -r win-x64 --self-contained true
```
