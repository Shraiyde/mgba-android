# Android peer-link foundation

This module adds a direct TCP peer transport to the Android mGBA frontend.

## Current protocol
- One device listens; the other connects to its reachable IP address and port.
- The four-digit PIN is checked during a small versioned handshake.
- Frames carry a message type, payload length, and sequence number.
- Reserved game message types cover overworld position, shared story flags,
  co-op battle state, battle actions, battle results, keepalive, and chat.
- The transport runs on a worker thread and exposes a polling event queue to
  the SDL/main thread.

## Important network boundary
A PIN authenticates a connection; it does not discover a remote device or
create an Internet route. This first transport works on a LAN and across the
Internet when the hosting phone is directly reachable (for example, global
IPv6 or a forwarded port). Carrier-grade NAT and many home routers prevent
unsolicited inbound connections. A reliable PIN-only Internet rendezvous
without any reachable endpoint or intermediary is not technically possible.
No cloud service or relay is included here.

## Security status
The initial transport is a development foundation, not production-secure:
the PIN handshake is plaintext and the TCP stream is not encrypted. Do not
use it on untrusted networks until authenticated encryption and a stronger
pairing credential are added. A four-digit PIN alone is too weak as an
Internet security credential.

## Game integration
The message IDs are transport-level envelopes only. The modified Heart & Soul
ROM still needs an explicit mailbox/protocol bridge so it can publish player
coordinates, shared story flags, battle snapshots, and player actions. Bag and
Pokémon Center/Mart inventory operations remain local to each game.
