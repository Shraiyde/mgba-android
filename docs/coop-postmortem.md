# Co-op phone-test postmortem (pre–World Host)

Short root-cause list from Stephen’s real-phone reports (APK ≤238 / ROM ≤128
era). Companion ROM detail: pokehns-expansion `docs/coop-presence-movement.md`.

| Symptom | Root cause |
| --- | --- |
| Invisible outdoors, visible indoors | `Coop_IsPeerVisible` required `remoteMapId != 0`; New Bark packs as `0` |
| Skipping / teleport while walking | Snap at Manhattan gap ≥ 2; POSITION sender used 2-deep `DiscardOldest`; collision refuse → snap |
| “Guessed” partner position | Spawned at local coords; receiver chased instead of replaying peer tile path |
| Vanish after battles / menus | Field return rebuilds sprites but skips link-player slots; stale `active` + dead sprite |
| Missing outdoors / after cutscenes | 16 object slots filled by NPCs; hide/reload could evict partner |
| Disconnects / stuck Relay | Phone-to-phone WebRTC + MQTT dual-use; weak TURN/NAT path; no durable host authority |
| URL / custom TURN prompts | Pairing UX asked for broker/TURN details instead of PIN-only |

**Direction since:** map-0 visibility fix, step queue + AbortPolicy POSITION, reserved object slots + CB1 watchdog, field-return resync, baked Metered TURN, PIN-only UI, PC World Host as WebRTC authority.
