# Android co-op audit (`mgba-android`)

Code-reading audit of the Heart & Soul Android app and co-op layer on
`master`. No emulator / runtime tests. Upstream SDL boilerplate TODOs in
`SDLActivity` / `SDLSurface` / HID helpers are out of scope unless they break
HnS co-op.

Companion ROM classification: [`pokehns-expansion` `docs/coop-flag-audit.md`](https://github.com/Shraiyde/pokehns-expansion/blob/master/docs/coop-flag-audit.md).  
Phone-failure RCA (short): [`coop-postmortem.md`](coop-postmortem.md).

---

## 1. Summary

The Android stack is **feature-complete for two-phone co-op**: 6-digit PIN
pairing, WebRTC Direct + MQTT relay, optional user TURN, AES-GCM frames,
mailbox v12 (position + on-change follower / flags / vars / config / ready /
resync), SIO link cable, Documents saves, portrait+landscape touch overlay,
and audio/frame pacing ownership off audio sync.

Remaining: very low-priority polish. Built-in Metered TURN is ON by default;
pairing only asks for the 6-digit PIN / companion code.

---

## 2. Protocol compatibility (ROM ↔ APK)

| Constant | Value | Both sides |
| --- | --- | --- |
| `HNS_COOP_MAILBOX_MAGIC` | `0x504F4F43` | ROM + Android |
| `HNS_COOP_MAILBOX_VERSION` | **13** | ROM + Android |
| `HNS_COOP_MAILBOX_SIZE` | 2296 | ROM + Android |
| `localFollower` / `remoteFollower` | 2284 / 2288 | ROM publishes local; Android copies peer → remote |
| `localPresenceResync` | 2292 | ROM bumps on field return; Android forces POSITION + asks peer |
| Flag / var queues | 128 / 128 | ROM + Android |
| Peer frame `PEER_VERSION` | 1 | Android peer link |
| Wire message IDs | POSITION=16 … WORLD_RESYNC=30 | Android; ROM via mailbox words |

Mailbox **v13** keeps v12 follower packing and adds `localPresenceResync`
@2292 so field return (battle/menu/warp) forces a mutual POSITION refresh.
POSITION stays 6 words steady-state; a 7th word carries follower only on change. PIN length is an Android MQTT/WebRTC
pairing concern only (not in the GBA mailbox). Old 4-digit saved sessions
must re-pair with a new 6-digit PIN.

---

## 3. Areas reviewed

| Area | Status |
| --- | --- |
| Direct WebRTC / Relay MQTT, PIN, reconnect, Direct retry | PC World Host SoT (v3 sender 2); PIN-only; baked TURN |
| Mailbox I/O | Replace-same-id on full ring + WORLD_RESYNC request |
| Avatar / presence | Solid; resume re-sync; v12 follower; POSITION AbortPolicy queue (no DiscardOldest) |
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
| P1 | No TURN path | Built-in Metered TURN ON by default; custom TURN UI removed |
| P1 | Verbose diagnostic spam | `PREF_DIAG_LOGS` + `nativeSetDiagLogs` |
| P2 | Unused BATTLE_*/CHAT opcodes | Removed from enum; 18–22 reserved; `WORLD_RESYNC=30` |
| P2 | Hide-toggle vs R / stick clamp | Prior pass |

---

## 5. Remaining

| Pri | Issue | Notes |
| --- | --- | --- |
| — | Custom TURN UI | Removed; stale prefs cleared on attach |
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
