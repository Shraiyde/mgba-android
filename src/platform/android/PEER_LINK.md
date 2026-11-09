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

The ROM exposes an EWRAM mailbox identified by `COOP` magic. The Android frontend writes the peer-connected state into it. The game then starts its existing GBA link protocol and returns to the field in link mode.

The ROM changes currently:
- Spawn and update the existing link-player objects in the overworld
- Check both linked players for NPC trainer sight, so either player can trigger an encounter
- Convert ordinary NPC trainer battles into two-player link double battles
- Keep wild battles local
- Permit each player to use their own bag during co-op battles
- Use the existing link battle controllers and party exchange instead of creating a second battle engine
- Exchange an initial snapshot of shared story flags, then propagate newly set Johto/Kanto story, gym, badge, and major-boss flags
- Keep item pickups, bag contents, Pokémon, and personal service unlocks local to each player

## Remaining game work

A full audit of special scripted trainer battles is still needed. Map transitions, battle rewards, and disconnect recovery have not been verified in-game.

The user requested that no build or test be run. These changes are committed to the existing repositories but have not been compiled or tested.
