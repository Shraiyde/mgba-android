# Heart & Soul PC Co-op Server

This is the Windows relay server for the Android mGBA clients.

## What it does

- Starts a co-op room and displays a four-digit PIN.
- Lets both Android clients join the same room with that PIN.
- Authenticates clients using PBKDF2-HMAC-SHA256 and HMAC-SHA256.
- Pairs the two phones and forwards their encrypted peer frames.
- Sends heartbeats and detects disconnected clients.
- Keeps the same room session alive while a phone reconnects.

The server forwards opaque encrypted frames. It does not load ROMs, read save files, or interpret Pokémon game data.

## Network requirements

The server makes an **outbound** MQTT connection to `broker.emqx.io:1883`. The Android clients use the broker's secure WebSocket endpoint. No inbound router ports, port forwarding, public IP, or Tailscale setup is required.

Both phones and the PC need internet access. The relay depends on the public EMQX broker being available.

## Run

Install the .NET 8 Desktop Runtime only if you are running the framework-dependent build. The GitHub Actions build is self-contained and does not require a separate .NET installation.

Run `HnsPcServer.exe`. It generates a four-digit PIN and displays it in the console. Enter that PIN in the PC Server option on both phones.

You can also choose a PIN manually:

```powershell
HnsPcServer.exe --pin 0427
```

Optional broker override:

```powershell
HnsPcServer.exe --broker broker.emqx.io
```

Keep the server window open while playing. Press Ctrl+C to stop it.

## Build locally

```powershell
dotnet publish .\server\windows\HnsPcServer.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -o .\publish
```

The executable will be `publish\HnsPcServer.exe`.

## Protocol

The relay uses the existing Android `PeerLinkManager` PC-server protocol under `hns/johto-pc/{PIN}/{session}`. The server validates the PIN-derived proof, exchanges the clients' public-key metadata, and forwards encrypted frames without decrypting them.
