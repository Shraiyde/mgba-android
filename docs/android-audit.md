# Android co-op audit (`mgba-android`)

Code-reading audit of the Heart & Soul Android app and co-op layer on
`master`. No emulator / runtime tests. Upstream SDL boilerplate TODOs in
`SDLActivity` / `SDLSurface` / HID helpers are out of scope unless they break
HnS co-op.

Companion ROM classification: [`pokehns-expansion` `docs/coop-flag-audit.md`](https://github.com/Shraiyde/pokehns-expansion/blob/master/docs/coop-flag-audit.md).

---

## 1. Summary

The Android stack is **feature-complete for two-phone co-op**: PIN pairing,
WebRTC Direct + MQTT relay, AES-GCM frames, mailbox v11 (position / flags /
vars / config / ready), SIO link cable, Documents saves, portrait+landscape
touch overlay, and audio/frame pacing ownership off audio sync.

This pass fixed the highest-impact reliability and lifecycle gaps found in
audit. Remaining items are infrastructure (TURN), optional polish, or
intentionally deferred protocol stubs.

---

## 2. Areas reviewed

| Area | Status |
| --- | --- |
| Direct WebRTC / Relay MQTT, PIN, reconnect, Direct retry | Solid; Direct STUN-only (no TURN) |
| Mailbox I/O (flag/var/position/config/SIO) | Fixed send-queue split; SIO_RESYNC now sent on overflow |
| Avatar / presence across maps | Solid; resume forces story/presence re-sync |
| Battle disconnect (emulator side) | SIO `0xFFFF` + presence clear; ROM owns clean abort |
| Saves outside the app | Documents path + re-prompt if still denied |
| Touch controls / layout | Stick clamp + hide-toggle moved off R |
| Audio / frame pacing | Intentional; diagnostics still verbose |
| Settings / UI flows | LINK dialog + SAF ROM picker |
| Crash / lifecycle / threading | WebRTC uses Application context; FGS notification permission |
| Permissions | INTERNET, FGS, MANAGE_EXTERNAL_STORAGE, POST_NOTIFICATIONS |
| Project TODO/FIXME | None material in HnS-owned files |

---

## 3. Fixed this pass

| Pri | Issue | Fix |
| --- | --- | --- |
| P0 | POSITION/mailbox shared `DiscardOldest` queue could drop already-“sent” flags/vars | Separate `positionSender` (DiscardOldest×2), `mailboxSender` (AbortPolicy×64), existing SIO AbortPolicy |
| P0 | WebRTC init NPE after `detachActivity()` | `appContext` Application context for `PeerConnectionFactory` |
| P1 | Pause froze emulation → peer presence expired with no recovery on resume | `PeerLinkManager.onActivityResumed()` → `nativeRequestStoryResync()` clears presence/story-ready and boosts drain |
| P1 | Missing `POST_NOTIFICATIONS` (API 33+) for FGS | Manifest + one-shot runtime request |
| P1 | `mPEER_MSG_SIO_RESYNC` receive-only | On SIO queue overflow, send resync jump to min remaining seq; set `sioQueueOverflow` |
| P1 | PBKDF2 500k on UI thread when hosting | Derive PIN key / ECDH on `cryptoExecutor` |
| P1 | Saves “Later” never re-prompted | Re-ask every 3 days (max 5 declines) until all-files access granted |
| P1 | FGS start with null Activity | Prefer `appContext` for `PeerLinkKeepAliveService.start` |
| P2 | Hide-controls toggle overlapped top-right **R** | Move toggle to bottom-right |
| P2 | Portrait stick unclamped when game view null | Always clamp stick inside window |

Also kept from prior reconnect work: story-ready clear on session reopen, peer-return POSITION boost, duplicate mailbox entry skip, Direct auto-retry.

---

## 4. Remaining

| Pri | Issue | Notes |
| --- | --- | --- |
| P1 | No TURN servers | Many cellular NATs stay on MQTT QoS 0 relay; needs infra, not code-only |
| P1 | Incoming flag/var ring full still drops | Mitigated by AbortPolicy backpressure on send + reconnect requeue; optional NACK later |
| P2 | Unused opcodes `BATTLE_*` / `KEEPALIVE` / `CHAT` | ROM battle abort is mailbox/SIO based; stubs intentional |
| P2 | Debug overlay draw is no-op but diagnostics still logged | Gate behind build flag if log noise matters |
| P2 | Public EMQX + 4-digit PIN entropy | Mitigated by PBKDF2; longer codes need product change |
| P2 | `threadedVideo` forced off | Documented diagnostic choice |
| — | Upstream SDL TODOs | Ignored |

---

## 5. What already looked solid (unchanged)

- E2E crypto (PBKDF2 → ECDH → AES-GCM, nonce domains, key rotate hygiene)
- SIO hold/retransmit without fabricating `0xFFFF` while connected
- Presence sticky across local map loads (`localSioActive` gate)
- Frame pacing at 59.7275 Hz with EGL swap interval 0 / audioSync off
- SAF ROM import; `configChanges` + `fullSensor` avoids rotation teardown
- Keep-alive FGS while co-op session is active
