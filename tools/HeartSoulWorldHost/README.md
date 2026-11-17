# Heart & Soul — World Host (Windows)

**ZeroTier / Tailscale Minecraft-server model.** The PC listens for **plain TCP**
from both phones on **all interfaces**. There is **no MQTT**, **no WebRTC**,
**no TURN**, and **no UPnP / public port-forward**.

Stephen’s router cannot be configured and blocks inbound ports. Overlay VPN
(ZeroTier preferred, Tailscale OK) is the path.

## Quick start (Stephen)

1. Leave **ZeroTier** connected (IP should be `10.137.178.161`). Tailscale optional.
2. Unzip the release and run `Start-WorldHost.bat`.
3. Approve the **Windows Firewall** UAC prompt once if asked.
4. Note **ROOM PIN** and the printed ZeroTier / Tailscale addresses.
5. On each phone: Co-op → **Join PC Host** → enter the PIN.
   Host address defaults to `10.137.178.161:27770` (remembered; advanced field only if needed).

Status on phones: `Connected to PC host` or `Reconnecting…`.

PIN file: `%LOCALAPPDATA%\HeartSoulWorldHost\host.json`  
`--new-pin` rotates the PIN.

## Petra — phone ZeroTier setup

1. Install **ZeroTier One** (Play Store).
2. **Join Network** using Stephen’s ZeroTier network ID.
3. Keep ZeroTier on while playing.
4. Open Heart & Soul → Co-op → **Join PC Host** → type the PIN from Stephen’s console.
5. Leave the advanced host field as `10.137.178.161:27770` unless Stephen says otherwise.

## Build

```bat
dotnet publish HeartSoulWorldHost.csproj -c Release -r win-x64 --self-contained true
```
