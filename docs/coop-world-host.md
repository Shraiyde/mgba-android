# Co-op World Host (ZeroTier)

Stephen’s network: **no router login, no UPnP, inbound ports closed**. The PC
runs **ZeroTier** (`10.137.178.161`). Phones join that network and connect
**directly** — credentials are **built into the APK and the host** (obfuscated
shared secret; never logged).

```
Phone A ──TCP──┐
               ├── HeartSoulWorldHost.exe  (TCP fixed port, all interfaces)  ← SoT
Phone B ──TCP──┘
         ZeroTier
```

## Hard rules

- **No MQTT**, **no WebRTC**, **no TURN**, **no relay**, **no UPnP**.
- **No PIN / address typing** on phones — one tap **Join PC Host**.
- Host address, port, and shared auth secret are baked into APK + host.
- Status: **Connected to PC host** / **Reconnecting…** (once each) / **PC host unreachable** (terminal).
- One persistent TCP link with **1 s** host heartbeats and **10 s** soft-resume tolerance.
- Reconnect uses backoff; never flaps; never opens a second socket while linked.
- Host replaces a stale socket for the same phone instead of kicking both.

## Petra — ZeroTier on the phone

1. Install **ZeroTier One** (Play Store).
2. **Join Network** with Stephen’s ZeroTier network ID.
3. Keep ZeroTier connected while playing.
4. Open Heart & Soul → Co-op → **Join PC Host** (one tap).

## Stephen — PC host

1. Keep ZeroTier connected.
2. Run `Start-WorldHost.bat`. Approve the one-time Windows Firewall UAC if prompted.
3. Leave the console open. Phones need no PIN.
