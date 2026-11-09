# Android peer-link

The Android app supports a four-digit PIN room hosted by the Windows PC server:

1. Run `HeartSoulCoopServer.exe` on the PC. It displays the room PIN.
2. Both phones choose **PC Server** in the co-op menu and enter that same PIN.
3. The PC server authenticates both clients and forwards encrypted game-link frames between them.
4. Both phones exchange ephemeral ECDH keys and use AES-GCM for end-to-end frame encryption.
5. The PC and phones reconnect automatically after temporary network loss.

The PC makes an outbound MQTT connection to the public EMQX broker; the phones use secure MQTT WebSockets to the same broker. This avoids requiring an inbound port or router port-forward. The PC server is the room coordinator and relay; both phones still run their own game.

## Transport implementation

- `PeerLinkManager.java` owns the PC-server PIN join flow, MQTT signaling, encrypted relay frames, and reconnect behavior.
- The PIN-derived PBKDF2/HMAC handshake authenticates the exchanged ephemeral ECDH keys.
- Peer frames are encrypted with AES-GCM before using either transport.
- `android-peer-jni.c` passes encrypted peer frames between Java and native mGBA.
- `android-peer-link.c` carries framed messages and queues GBA serial words.
- `android-link-cable.c` exposes the peer transport as the GBA multiplayer SIO driver.

## Heart & Soul ROM integration

The ROM exposes a version 8 EWRAM mailbox identified by `COOP` magic. The Android frontend writes peer-connected state into it, relays shared story flags and variables, exchanges map IDs and whether each game is actively running the link field protocol; the ROM uses that remote activity bit to avoid triggering co-op trainer battles while the partner is in a solo battle, and acknowledges the initial story snapshot. The game starts its existing GBA link protocol and returns to the field in link mode. Co-op trainer battles are enabled only while both peers report the same map. During solo wild or scripted battles, the battle side advertises that it is not exchanging overworld SIO input, so the other player does not wait on a missing serial transfer; the network session remains connected.

The version 8 mailbox also carries the host's challenge configuration. The host publishes all randomizer option bits, Nuzlocke mode and clause bits, the active/end Nuzlocke flags, and the randomizer seed. The joining game applies that snapshot to its own challenge settings and overrides its randomizer seed at runtime (including deterministic handling for CHAOS mode), while retaining its own trainer identity, party, bag, and save. The host waits for a config acknowledgement before co-op trainer battles are enabled; the client reapplies a new snapshot after reconnect. The Android bridge relays these config packets only from host to client.

The ROM changes currently:
- Spawn and update the existing link-player objects in the overworld
- Check both linked players for NPC trainer sight, so either player can trigger an encounter
- Render the remote player with the boy/girl avatar selected on that player's save
- Exchange map identity so trainer co-op is restricted to peers on the same map
- Convert ordinary NPC trainer battles into two-player link double battles
- Keep wild battles local
- Permit each player to use their own bag during co-op battles
- Use the existing link battle controllers and party exchange instead of creating a second battle engine
- Exchange an initial snapshot of shared story flags, then propagate newly set Johto/Kanto story, gym, badge, and major-boss flags
- Keep item pickups, bag contents, Pokémon, and personal service unlocks local to each player

## Remaining game work

A targeted source audit and CI ROM build have been completed. The latest Heart & Soul co-op ROM build is being rebuilt after the non-HNS compatibility fix. Runtime verification is still pending for special scripted trainer battles, map transitions, battle rewards, and disconnect/reconnect behavior during active battles. The Android APK and Windows relay server have separate successful CI builds; a two-phone end-to-end session has not yet been exercised.
