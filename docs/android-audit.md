# Android co-op audit (`mgba-android`)

Code-reading audit of the Heart & Soul Android app and co-op layer on
`master`. No emulator / runtime tests. Upstream SDL boilerplate TODOs in
`SDLActivity` / `SDLSurface` / HID helpers are out of scope unless they break
HnS co-op.

Companion ROM classification: [`pokehns-expansion` `docs/coop-flag-audit.md`](https://github.com/Shraiyde/pokehns-expansion/blob/master/docs/coop-flag-audit.md).

---

## 1. Summary

The Android stack is **feature-complete for two-phone co-op**: 6-digit PIN
pairing, WebRTC Direct + MQTT relay, optional user TURN, AES-GCM frames,
mailbox v11 (position / flags / vars / config / ready / resync), SIO link
cable, Documents saves, portrait+landscape touch overlay, and audio/frame
pacing ownership off audio sync.

Remaining: optional TURN **infra** (user must supply a TURN URI; none is
bundled), and very low-priority polish.

---

## 2. Protocol compatibility (ROM ↔ APK)

| Constant | Value | Both sides |
| --- | --- | --- |
| `HNS_COOP_MAILBOX_MAGIC` | `0x504F4F43` | ROM + Android |
| `HNS_COOP_MAILBOX_VERSION` | **11** | ROM + Android |
| `HNS_COOP_MAILBOX_SIZE` | 2284 | ROM + Android |
| Flag / var queues | 128 / 128 | ROM + Android |
| Peer frame `PEER_VERSION` | 1 | Android peer link |
| Wire message IDs | POSITION=16 … WORLD_RESYNC=30 | Android; ROM via mailbox words |

ROM build **hns-coop-125+** and APK builds from this audit closeout share
mailbox **v11**. PIN length is an Android MQTT/WebRTC pairing concern only
(not in the GBA mailbox). Old 4-digit saved sessions must re-pair with a new
6-digit PIN.

---

## 3. Areas reviewed

| Area | Status |
| --- | --- |
| Direct WebRTC / Relay MQTT, PIN, reconnect, Direct retry | Solid; optional TURN field (default off) |
| Mailbox I/O | Replace-same-id on full ring + WORLD_RESYNC request |
| Avatar / presence | Solid; resume re-sync |
| Battle disconnect | SIO + ROM abort; no separate BATTLE_* opcodes |
| Saves outside the app | Documents + periodic re-prompt |
| Touch controls | Stick clamp + hide-toggle off R |
| Audio / frame pacing | Intentional |
| Diagnostic logs | Gated behind LINK → Debug logs toggle |
| Permissions | INTERNET, FGS, MANAGE_EXTERNAL_STORAGE, POST_NOTIFICATIONS |

---

## 4. Fixed (this closeout + prior harden pass)

| Pri | Issue | Fix |
| --- | --- | --- |
| P0 | Shared DiscardOldest could drop flags/vars | Separate POSITION / mailbox / SIO senders |
| P0 | WebRTC NPE after Activity detach | Application context |
| P1 | Pause → presence expiry | Resume story/presence re-sync |
| P1 | Missing POST_NOTIFICATIONS | Manifest + runtime ask |
| P1 | SIO_RESYNC receive-only | Send on queue overflow |
| P1 | Inbound mailbox ring-full silent drop | Replace same flag/var id in ring; else `WORLD_RESYNC` + local requeue |
| P1 | 4-digit PIN entropy | **6-digit** PIN + selectable host PIN dialog |
| P1 | No TURN path | Optional TURN URI/user/pass prefs (default empty = STUN only) |
| P1 | Verbose diagnostic spam | `PREF_DIAG_LOGS` + `nativeSetDiagLogs` |
| P2 | Unused BATTLE_*/CHAT opcodes | Removed from enum; 18–22 reserved; `WORLD_RESYNC=30` |
| P2 | Hide-toggle vs R / stick clamp | Prior pass |

---

## 5. Remaining

| Pri | Issue | Notes |
| --- | --- | --- |
| P1 | TURN **infrastructure** | App accepts a user-supplied `turn:` URI. Free public STUN is built in. A working TURN still needs a host you control (e.g. coturn) or a free-tier TURN provider account — **not** bundled or paid-for here. |
| P2 | Public EMQX broker | Shared public broker; 6-digit PIN + PBKDF2 mitigate; private broker would be stronger |
| P2 | `threadedVideo` forced off | Intentional diagnostic choice |
| — | Upstream SDL TODOs | Ignored |

---

## 6. What already looked solid

- E2E crypto (PBKDF2 → ECDH → AES-GCM)
- SIO hold/retransmit without fabricating `0xFFFF` while connected
- Presence sticky across local map loads
- Frame pacing at 59.7275 Hz / audioSync off
- SAF ROM import; Keep-alive FGS
