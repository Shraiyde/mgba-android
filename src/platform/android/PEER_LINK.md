# Android peer-link

The Android frontend now has a framed peer transport and an mGBA GBA
serial-link driver. The serial driver maps GBA multiplayer transfers onto
peer messages so Heart & Soul can use its existing link-player and link-battle
systems rather than requiring a second battle engine.

## Connection design

The intended phone-to-phone flow is:
- Host creates a short-lived room and receives a four-digit pairing PIN.
- Joiner enters only that PIN; the app resolves the peer through rendezvous.
- ICE/NAT traversal attempts a direct peer-to-peer data channel first.
- A relay path is used automatically when direct connectivity is unavailable.
- The app retains the pairing and reconnects after temporary network loss.

The user-facing PIN-only rendezvous and relay transport are still being
implemented. The current socket transport is an intermediate development
transport and requires a reachable address; it is not the final connection
flow.

## GBA serial bridge

- `android-link-cable.c` registers a `GBASIODriver` on
  `mPERIPH_GBA_LINK_PORT`.
- Multiplayer SIO transfers exchange each device's 16-bit send word.
- Host is GBA player 0; joiner is GBA player 1.
- The bridge currently handles GBA MULTI mode. It is the transport layer for
  the ROM's existing cable-link protocol, not the complete persistent
  overworld co-op implementation.

## Game integration still required

The Heart & Soul ROM must start and maintain the link session outside the
Cable Club, synchronize both players across ordinary map transitions, and
route trainer-battle actions so each human controls their own battler. Shared
story flags must be distinguished from personal events; wild battles and
personal inventories remain local.
