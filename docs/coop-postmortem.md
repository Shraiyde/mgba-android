# Co-op phone-test postmortem (pre–World Host wave)

Honest root-cause analysis of failures Stephen hit on **real phones**
(APK ≤238 / ROM ≤128 era and the patches that followed). No emulator runs
were used to find these; they came from field reports + code reading.

Companion ROM notes: [`pokehns-expansion` `docs/coop-presence-movement.md`](https://github.com/Shraiyde/pokehns-expansion/blob/master/docs/coop-presence-movement.md).  
Current host model: [`coop-world-host.md`](coop-world-host.md).

---

## 1. Symptom → design choice → why it shipped

| Field symptom | Design choice that caused it | Why testing missed it |
| --- | --- | --- |
| **Partner invisible outdoors, visible indoors** | ROM `Coop_IsPeerVisible` required `remoteMapId != 0`. Packed map id is `(group) \| (num << 16)`. **New Bark = 0/0 → 0**, so outdoors looked “unpublished”; indoor maps have nonzero groups and passed. | Static review treated `0` as “unset.” No fixture asserting New Bark / map 0 is a valid same-map id. Phone tests that stayed indoors looked fine. |
| **Skipping / teleporting while walking** | (1) Receiver snapped when Manhattan gap ≥ **2** tiles. (2) Android POSITION sender used a **2-deep `DiscardOldest`** queue under WebRTC load. (3) Collision could refuse a one-tile walk; next frame snapped again. | CI never stressed ~70 ms RTT + run speed. Queue policy was chosen for “don’t block the UI thread,” not for tile fidelity. No counter for dropped POSITION frames. |
| **“Guessed” partner position** | Spawn used **local** avatar coords until the first remote fix; receiver **dead-reckoned** a chase step instead of replaying the peer’s tile path. | Looked OK when both stood still or walked slowly. No assert that spawn X/Y equals last published remote coords. |
| **Partner vanishes after battles / menus** | `SpawnObjectEventsOnReturnToField` rebuilds map sprites but **skips link-player slots**. Presence entry stayed `active` with a **dead sprite**; spawn logic thought the avatar still existed. | Battle → field return is the common path on phones; agent policy forbade emulator playthroughs, and no static check validated “active link object ⇒ live sprite.” |
| **Partner missing outdoors under NPC density / after cutscenes** | `OBJECT_EVENTS_COUNT == 16`. Dense outdoor views filled every slot; spawn bailed. Cutscene `RemoveObjectEvent` / hide / reload could evict whatever slot the partner used. | Indoor maps are sparse → “works in the lab.” No reserved slot; no every-frame “same map but missing” watchdog. |
| **Disconnects / stuck Relay / flaky Direct** | Phone-to-phone WebRTC without a durable host; MQTT used as both signaling **and** game-data backup; ICE often needed TURN on cellular/CGNAT; heartbeats/grace windows were easy to mis-tune; pause/resume cleared presence. | Happy-path Wi-Fi LAN paired fine. Cellular + NAT + app backgrounding were under-tested. Status text did not always say *why* Direct failed. |
| **User-facing URL / custom TURN prompts** | Early pairing UX asked for broker/TURN details “for power users,” assuming Stephen would paste Metered credentials. | Usability was never treated as a hard ship gate. Real users hit a wall before gameplay. Fixed by baking Metered STUN/TURN and removing the prompt; World Host is PIN-only. |

---

## 2. Deeper causes (patterns, not one-liners)

1. **Sentinel collision with real game data** — Using `0` for “missing map” when a real map packs to `0`.
2. **Queueing for convenience, not correctness** — `DiscardOldest` on POSITION trades latency for silent tile loss.
3. **Link-player objects are a second sprite system** — Vanilla return-to-field / OOB cull / hide scripts were written for map NPCs; presence avatars were bolted on without a lifecycle owner.
4. **No reserved resources** — Partner competed with outdoor NPCs for 16 object-event slots.
5. **P2P authority on phones** — Two battery-constrained endpoints + NAT made every hiccup a “disconnect,” with no PC-side authority until World Host.
6. **Ship gates were compile/CI only** — Explicit “no runtime testing” for agents meant phone bugs were the first integration test.

---

## 3. What we changed (mapping back)

| Fix | Addresses |
| --- | --- |
| Visibility gated on `remoteMapReady` + map equality (allow map id `0`) | Outdoor New Bark invisibility |
| Step queue (24); snap only if gap ≥ 8 or map change; force step if collision refuses | Skipping / teleport |
| Spawn at remote X/Y; no chase invent | Guessed position |
| POSITION queue depth 32 + **`AbortPolicy`** (retry, don’t drop) | Skipping under load |
| Invalidate presence on field return; mailbox v13 `localPresenceResync` | Post-battle / menu vanish |
| Reserved slots `COUNT-1` / `COUNT-2`; block eviction; CB1 watchdog re-spawn | Outdoor density + cutscenes + warps |
| Built-in Metered TURN; remove URL/TURN prompts | Pairing friction |
| **PC World Host** (WebRTC authority, MQTT signaling only, PIN join) | Disconnect / authority class |

---

## 4. Testing gaps (be explicit)

| Gap | Consequence |
| --- | --- |
| No emulator / device CI for two-client overworld | Map-0, battle return, warp, and cutscene hide never failed a build |
| No POSITION drop / gap metrics in logs | Skipping looked like “Netcode is hard” instead of “queue discarded tiles” |
| No assert on link-object sprite liveness | Stale `active` + dead sprite shipped |
| No object-slot occupancy dump when spawn fails | Outdoor “invisible” blamed on maps, not slots |
| Pairing UX not on the acceptance checklist | URL/TURN prompts reached Stephen |
| Relay vs Direct not distinguished in shareable logs | Disconnect reports lacked ICE/candidate-type evidence |

---

## 5. Checklist for Stephen’s phone tests (send logs back)

Goal: next failure is **diagnosable from a log paste**, without us running emulators.

### 5.1 Before a session (static / setup)

- [ ] Same wave artifacts: matching **ROM + APK + World Host** tags from one report (e.g. `hns-coop-N`, `android-coop-N`, `world-host-N`).
- [ ] Both phones: install APK, load ROM, **Co-op → Debug logs: ON**.
- [ ] Prefer **Join PC Host (PIN)** with World Host running; note status `PC Host (Direct)` vs `PC Host (TURN)`.
- [ ] Record phone models, Android versions, Wi-Fi vs cellular, and whether both are on the same LAN as the PC.

### 5.2 Capture method

On each phone (USB debugging or wireless):

```bash
adb logcat -c
adb logcat -v time SDL:V GBA:V PeerLink:V HnsCoop:V *:S > hns-phoneA.log
```

Reproduce once, then Ctrl+C and send the file (or the 2–3 minutes around the glitch).  
On-screen co-op status lines are also useful — screenshot when it says Relay / Direct / TURN / Waiting.

World Host console on the PC: copy the window output for the same time window (`[player]`, `[webrtc]`, `[room]`).

### 5.3 Scenario matrix (one row per bug class)

| # | Do this | Pass look | If fail, paste / note |
| --- | --- | --- | --- |
| A | Both stand in **New Bark** outdoors | Both see each other + followers | local/remote map ids (expect `0`), `peerLinkReady`, transport status |
| B | Run in a straight line ~10 tiles | Smooth walk, no teleport | Any POSITION gaps; status flipping Relay↔Direct |
| C | Enter a building, exit, cross a route connection | Partner stays visible on same map | Map id changes; presence-resync bumps |
| D | Wild or trainer battle → return to field | Partner back **immediately** | “active but invisible”; resync; watchdog respawn |
| E | Open bag / party / PC / save, close | Partner still there | Same as D |
| F | Fly / Surf / fade / short cutscene | Partner returns when control returns | Object hide / slot eviction |
| G | Toggle airplane 10s on one phone | Reconnect; both visible again | World Host `[player] reconnect`; phone status text |
| H | Background the app 30s, resume | Presence restored | Resume re-sync; heartbeat age |

### 5.4 What to grep in logs (send these hits)

| Look for | Meaning |
| --- | --- |
| `PC Host (Direct)` / `PC Host (TURN)` / `Relay` | Which transport carried game data |
| `presence-resync` / `localPresenceResync` / story resync | Field-return path fired |
| `GBA diagnostic:` (with Debug logs ON) | Frame pacing / coopMs spikes during skips |
| `WebRTC` / `ICE` / `TURN` / `data channel` | Direct path health |
| World Host: `ICE connected`, `data channel open`, `Both phones ready` | Host saw both clients |
| Any `refused`, `offline`, `dropped`, `queue` | Auth / heartbeat / send-path failure |

### 5.5 Static checks maintainers should run before tagging a build

Code-only; no emulator:

- [ ] Mailbox **version + size** match ROM header ↔ `android-main.c` ↔ docs.
- [ ] `Coop_IsPeerVisible` must **not** require `remoteMapId != 0`.
- [ ] POSITION sender is **`AbortPolicy`** (or equivalent no-silent-drop), depth ≥ 32.
- [ ] Presence spawn uses **remote** coords; snap threshold ≥ 8; step queue present.
- [ ] Field return **invalidates** presence link objects; CB1 watchdog can re-spawn same tick.
- [ ] Partner / remote follower use **reserved** object-event ids; `GetFirstInactive` / `GetAvailable` skip them; `RemoveObjectEvent` cannot steal them.
- [ ] Pairing UI: **PIN only** — no broker URL or custom TURN fields on the happy path.
- [ ] World Host / phone share the same session-key string (`…\|HNS-WORLD-HOST-V1`) and signal JSON shape (`sdp` / `candidate`).
- [ ] APK and World Host **compile** on the same master tip (missing Java helpers already broke one wave).

### 5.6 Minimal report template (copy/paste)

```text
Artifacts: ROM ___ / APK ___ / World Host ___
Phones: A=___  B=___
Network: wifi/cell, same LAN as PC? ___
Mode: PC Host Direct | PC Host TURN | phone Direct | Relay
Scenario letter: A–H ___
What I saw:
What partner saw:
Attached: hns-phoneA.log, hns-phoneB.log, world-host-console.txt, screenshot(s)
```

---

## 6. Takeaway

Most phone failures were **not** “WebRTC is flaky” in the abstract. They were
specific invariants we violated (map id 0, discard queues, link-sprite
lifecycle, slot exhaustion, P2P authority, prompt-heavy pairing) and then
could not catch because builds only proved **compile**, not **two-phone
overworld**.

World Host + reserved slots + CB1 watchdog + AbortPolicy POSITION close the
known classes. Stephen’s checklist above is how the next unknown class gets
a log-backed root cause on the first report.
