# Co-op World Host (online)

Phones connect **directly** over public TCP to the World Host (Fly.io Frankfurt).
Credentials are **built into the APK and the host** (obfuscated shared secret;
never logged). No ZeroTier on phones.

```
Phone A ──TCP──┐
               ├── HeartSoulWorldHost  (TCP :27770)  ← SoT
Phone B ──TCP──┘
         public internet (Fly.io)
```

## Hard rules

- **No MQTT**, **no WebRTC**, **no TURN**, **no relay**, **no UPnP**.
- **No PIN / address typing** on phones — one tap **Join Online**.
- Host address, port, and shared auth secret are baked into APK + host.
- Status: **Connected to host** / **Reconnecting…** (once each) / **Host unreachable** (terminal).
- Persistent HUD: Host offline / connecting / connected + Partner waiting / online.
- One persistent TCP link with **1 s** host heartbeats and **10 s** soft-resume tolerance.
- First TCP connect timeout is **8 s** (covers ~2–3 s Fly cold starts).
- Reconnect uses backoff; never flaps; never opens a second socket while linked.
- Host replaces a stale socket for the same phone instead of kicking both.

## Petra — phone

1. Install the latest co-op APK.
2. Open Heart & Soul → Co-op → **Join Online** (one tap).
3. If the host was idle, wait a few seconds for cold start.

## Stephen — online host (Fly)

1. App `heartsoul-host` in `tools/HeartSoulWorldHost/fly.toml` (region `fra`).
2. Redeploy from Stephen’s side when the host binary changes.
3. Dedicated IPv4 + raw TCP port **27770** (same port as before).

## Optional — Windows host zip

`Start-WorldHost.bat` still builds for offline/LAN testing. Firewall UAC is
Windows-only; Linux/Fly skips it.
