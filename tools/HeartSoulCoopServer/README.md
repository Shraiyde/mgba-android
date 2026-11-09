# Heart & Soul Co-op PC Server

This is the Windows room host and relay for the two Android players. It does not run the ROM or emulate the game; both phones run their own copy of Heart & Soul while the PC coordinates the room and forwards encrypted GBA-link frames between them.

## Start

1. Download `HeartSoulCoopServer.exe` from the repository's **Releases** page.
2. Run the EXE on the Windows PC and leave its console window open.
3. Read the four-digit PIN displayed by the server.
4. On each Android device, open the co-op menu, choose **PC Server**, and enter that PIN.
5. Wait for the server to report that both players are connected.

## Network behavior

- The PC makes an outbound MQTT connection to the public EMQX broker.
- The phones use MQTT over secure WebSockets to the same broker.
- No inbound router port, port forwarding, Tailscale, or Cloudflare tunnel is required.
- The server routes encrypted game-link frames between the two registered Android clients.
- PIN-derived HMAC authenticates room joins; the phones exchange ephemeral ECDH keys and encrypt game frames end-to-end with AES-GCM.
- The server retains the same PIN in `%LOCALAPPDATA%\HeartSoulCoopServer\server.json`. The session cryptographic salt and session ID are regenerated each time the server starts.
- Run `HeartSoulCoopServer.exe --new-pin` to rotate the four-digit PIN.
- The server holds both registered client slots so the same two devices can reconnect after temporary network loss. Restarting the server starts a fresh session with the saved PIN.

## Build

The GitHub Actions workflow `.github/workflows/build-coop-server.yml` publishes a self-contained Windows x64 executable and attaches it to a GitHub Release. The EXE includes the .NET runtime and does not require a separate .NET installation.

## Important

The server is the relay and room coordinator, not a cloud-hosted game simulation. Keep the PC powered on and the server window running for the session to remain available.
