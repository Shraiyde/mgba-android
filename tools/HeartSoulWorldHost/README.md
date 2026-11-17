# Heart & Soul — World Host (Windows)

**ZeroTier Minecraft-server model.** This PC listens for **plain TCP** from both
phones. There is **no MQTT**, **no WebRTC**, and **no TURN**.

Host address, port, and join key are built into this EXE and the Android APK
(obfuscated). Phones use one tap: **Join PC Host**.

## Quick start

1. Put the PC and both phones on the **same ZeroTier network**.
2. Unzip the release and run `Start-WorldHost.bat` (or `HeartSoulWorldHost.exe`).
3. On each phone: Co-op → **Join PC Host**.
4. Leave the window open while you play.

Status on phones: `Connected to PC host` or `Reconnecting…`.

On start the host adds a Windows firewall allow rule for its TCP listen port.
If phones cannot connect, run the host once as Administrator, or allow the port
manually in Windows Defender Firewall.

## What the host owns

- Positions / followers / flags / vars / SIO order (claims in, host-stamped stream out)
- Direct TCP game sockets only
- Soft reconnect (~10 s); presence resync on return

## Build

```bat
dotnet publish HeartSoulWorldHost.csproj -c Release -r win-x64 --self-contained true
```
