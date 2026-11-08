# Android peer-link

The Android app now uses a four-digit PIN rendezvous flow:

1. One phone chooses **Host** and displays a four-digit PIN.
2. The other chooses **Join** and enters that PIN.
3. The apps establish a WebRTC data channel directly when NAT traversal succeeds.
4. If a direct route cannot be established, encrypted game frames automatically use the MQTT relay.
5. The host republishes its room advertisement and the clients reconnect after temporary network loss.

No PC-hosted server, Tailscale, or Cloudflare tunnel is used. The rendezvous/relay broker is the public EMQX MQTT WebSocket endpoint. WebRTC uses public STUN servers; MQTT is the fallback relay.

## Transport implementation

- `PeerLinkManager.java` owns the PIN UI, MQTT rendezvous, WebRTC data channel, relay fallback, and reconnect behavior.
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
