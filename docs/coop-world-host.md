# Co-op World Host (ZeroTier / Tailscale)

Stephen’s network: **no router login, no UPnP, inbound ports closed** except
router-owned 80/443. The PC already runs **ZeroTier** (`10.137.178.161`) and
Tailscale. Phones join that overlay and connect **directly** to the PC.

```
Phone A ──TCP──┐
               ├── HeartSoulWorldHost.exe  (TCP :27770 on all interfaces)  ← SoT
Phone B ──TCP──┘
         ZeroTier (preferred) or Tailscale
```

## Hard rules

- **No MQTT**, **no WebRTC**, **no TURN**, **no relay**.
- **No UPnP / public IP / port-forward** as the play path.
- Host listens on **all interfaces** (LAN + ZeroTier + Tailscale).
- **PIN** authenticates; phones use the host’s ZeroTier (or Tailscale) IP.
- Default phone host field: `10.137.178.161:27770` (remembered; advanced edit only if needed).
- Status: **Connected to PC host** / **Reconnecting…**.

## Petra — ZeroTier on the phone

1. Install **ZeroTier One** from the Play Store.
2. Open it and **Join Network** with Stephen’s ZeroTier network ID (he shares this once).
3. Leave ZeroTier connected while co-op is running.
4. Install the co-op APK. Co-op → **Join PC Host** → enter the **ROOM PIN** from the PC console.
5. Confirm the advanced host field shows `10.137.178.161:27770` (change only if Stephen’s ZeroTier IP changes).

## Stephen — PC host

1. Keep ZeroTier (and/or Tailscale) connected.
2. Run `Start-WorldHost.bat`. Approve the one-time Windows Firewall UAC if prompted.
3. Read **ROOM PIN**, **ZeroTier**, and **Tailscale** lines in the console.
4. Share the PIN with both phones.
