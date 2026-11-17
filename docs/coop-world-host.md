# Co-op World Host (ZeroTier direct)

Stephen’s framing: **the PC hosts the world; phones connect directly like a Minecraft server.**

```
Phone A ──TCP──┐
               ├── HeartSoulWorldHost.exe  (TCP baked port on ZeroTier)  ← SoT
Phone B ──TCP──┘
```

## Hard rules

- **No MQTT**, **no WebRTC**, **no TURN**, **no relay** of any kind for game data.
- Phones and PC are on the **same ZeroTier network**.
- Host address, port, and join key are **baked into the APK and the host** (obfuscated, never logged).
- Phones: one tap **Join PC Host** — no PIN, no URL dialog.
- Status: **Connected to PC host** / **Reconnecting…** (no flapping toasts).
- Soft resume: ~10 s drop tolerance; partner visibility held across brief reconnects.

## Roles

| Actor | Does | Must not |
| --- | --- | --- |
| **PC World Host** | Listen TCP; firewall rule; own positions/flags/SIO; stream host-stamped frames | Broker / TURN / MQTT |
| **Phone** | One-tap join; send claims; apply host frames (sender id 2) | Enter PIN / URL; use relay |

## Ops

1. Join ZeroTier on the PC and both phones.
2. Run `Start-WorldHost.bat` on the PC (opens Windows firewall for the listen port).
3. On each phone: Co-op → **Join PC Host**.
