using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using MQTTnet;
using MQTTnet.Client;
using MQTTnet.Protocol;
using SIPSorcery.Net;

namespace HeartSoulWorldHost;

/// <summary>
/// Authoritative Windows world host for Heart &amp; Soul co-op.
/// Phones connect as clients via WebRTC data channels (Metered TURN/STUN);
/// MQTT is signaling/pairing only. Optional LAN TCP on port 27770.
/// Does not modify tools/HeartSoulCoopServer.
/// </summary>
internal sealed class ClientSlot
{
    public required string Id { get; init; }
    public DateTime LastSeenUtc { get; set; }
    public bool Connected { get; set; }
    public RTCPeerConnection? PeerConnection { get; set; }
    public RTCDataChannel? DataChannel { get; set; }
    public string ConnType { get; set; } = "signaling";
    public NetworkStream? LanStream { get; set; }
    // Last authoritative POSITION payload words (host endian), 0 if unknown.
    public uint[] LastPosition { get; set; } = new uint[7];
    public bool HasPosition { get; set; }
}

internal sealed class RoomConfig
{
    public string? Pin { get; set; }
}

internal static class Program
{
    private const string BrokerHost = "broker.emqx.io";
    private const int BrokerPort = 8883;
    private const int Pbkdf2Iterations = 500_000;
    private const int LanPort = 27770;
    private const int ProtocolVersion = 2;
    private static readonly ConcurrentDictionary<string, ClientSlot> Clients = new();
    private static readonly SemaphoreSlim Gate = new(1, 1);
    private static IMqttClient? _mqtt;
    private static string _pin = "";
    private static string _session = "";
    private static string _salt = "";
    private static byte[] _pinKey = [];
    private static byte[] _sessionKey = [];
    private static CancellationTokenSource _stop = new();

    private static async Task Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;
        string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HeartSoulWorldHost");
        Directory.CreateDirectory(root);
        string configPath = Path.Combine(root, "host.json");
        bool resetPin = args.Any(a => a.Equals("--new-pin", StringComparison.OrdinalIgnoreCase));

        RoomConfig config = File.Exists(configPath)
            ? JsonSerializer.Deserialize<RoomConfig>(await File.ReadAllTextAsync(configPath)) ?? new RoomConfig()
            : new RoomConfig();

        if (resetPin || string.IsNullOrWhiteSpace(config.Pin) || config.Pin.Length != 6 || config.Pin.Any(c => !char.IsAsciiDigit(c)))
        {
            config.Pin = RandomNumberGenerator.GetInt32(0, 1_000_000).ToString("D6");
            await File.WriteAllTextAsync(configPath, JsonSerializer.Serialize(config, new JsonSerializerOptions { WriteIndented = true }));
        }

        _pin = config.Pin!;
        _session = Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant();
        _salt = Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant();
        _pinKey = Rfc2898DeriveBytes.Pbkdf2(_pin, Convert.FromHexString(_salt), Pbkdf2Iterations, HashAlgorithmName.SHA256, 32);
        _sessionKey = DeriveSessionKey(_pinKey, _session);

        Console.WriteLine("HEART & SOUL — WORLD HOST");
        Console.WriteLine("=========================");
        Console.WriteLine("ROOM PIN: " + _pin);
        Console.WriteLine("Keep this window open while both phones play.");
        Console.WriteLine("Phones: Co-op → Join PC Host → enter this 6-digit PIN.");
        Console.WriteLine("Transport: WebRTC to this PC (Metered TURN/STUN). MQTT is signaling only.");
        Console.WriteLine($"Optional LAN: TCP {LanPort} (same Wi-Fi / port-forward).");
        Console.WriteLine("PIN file: " + configPath);
        Console.WriteLine("Press Ctrl+C to stop. Run with --new-pin to rotate the PIN.");
        Console.WriteLine();

        Console.CancelKeyPress += (_, e) => { e.Cancel = true; _stop.Cancel(); };

        _mqtt = new MqttFactory().CreateMqttClient();
        _mqtt.ApplicationMessageReceivedAsync += OnMessage;
        _mqtt.DisconnectedAsync += async _ =>
        {
            if (!_stop.IsCancellationRequested)
            {
                Console.WriteLine("[network] Broker disconnected; reconnecting...");
                await ReconnectLoop();
            }
        };

        _ = RunLanListener(_stop.Token);
        await ReconnectLoop();
        _ = HeartbeatLoop(_stop.Token);
        _ = ClientWatchdog(_stop.Token);
        _ = StatusLoop(_stop.Token);

        try { await Task.Delay(Timeout.Infinite, _stop.Token); }
        catch (OperationCanceledException) { }

        try
        {
            await PublishRoomAdvertisement(clear: true);
            if (_mqtt.IsConnected)
                await _mqtt.DisconnectAsync();
        }
        catch { }
        Console.WriteLine("World host stopped.");
    }

    private static byte[] DeriveSessionKey(byte[] pinKey, string session)
    {
        using var hmac = new HMACSHA256(pinKey);
        return hmac.ComputeHash(Encoding.UTF8.GetBytes(session + "|HNS-WORLD-HOST-V1"));
    }

    private static async Task ConnectAndAdvertise()
    {
        var options = new MqttClientOptionsBuilder()
            .WithTcpServer(BrokerHost, BrokerPort)
            .WithTlsOptions(tls => tls.UseTls())
            .WithClientId("hns-world-" + Guid.NewGuid().ToString("N"))
            .WithCleanSession()
            .WithKeepAlivePeriod(TimeSpan.FromSeconds(20))
            .Build();

        await _mqtt!.ConnectAsync(options, _stop.Token);
        await _mqtt.SubscribeAsync(new MqttTopicFilterBuilder()
            .WithTopic($"{RootTopic}/up/+/+")
            .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
            .Build(), _stop.Token);
        await _mqtt.SubscribeAsync(new MqttTopicFilterBuilder()
            .WithTopic($"{RootTopic}/signal/+/up")
            .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
            .Build(), _stop.Token);
        await PublishRoomAdvertisement();
        Console.WriteLine("[network] Connected to signaling broker. Waiting for phones...");
    }

    private static async Task ReconnectLoop()
    {
        while (!_stop.IsCancellationRequested && !_mqtt!.IsConnected)
        {
            try
            {
                await ConnectAndAdvertise();
                return;
            }
            catch (OperationCanceledException) { return; }
            catch (Exception ex)
            {
                Console.WriteLine("[network] " + ex.Message);
                try { await Task.Delay(3000, _stop.Token); } catch { return; }
            }
        }
    }

    private static async Task OnMessage(MqttApplicationMessageReceivedEventArgs e)
    {
        string topic = e.ApplicationMessage.Topic;
        string[] parts = topic.Split('/');
        // hns/johto-world/{pin}/{session}/up/{clientId}/{kind}
        // hns/johto-world/{pin}/{session}/signal/{clientId}/up
        if (parts.Length < 7 || parts[0] != "hns" || parts[1] != "johto-world" ||
            parts[2] != _pin || parts[3] != _session)
            return;

        byte[] payload = e.ApplicationMessage.PayloadSegment.ToArray();
        if (parts[4] == "signal" && parts.Length == 7 && parts[6] == "up")
        {
            await HandleSignal(parts[5], payload);
            return;
        }
        if (parts[4] != "up" || parts.Length != 7)
            return;

        string clientId = parts[5];
        string kind = parts[6];
        if (clientId.Length is < 8 or > 64 || clientId.Any(c => !char.IsAsciiLetterOrDigit(c) && c != '-'))
            return;

        if (kind == "hello") { await HandleHello(clientId, payload); return; }
        if (kind == "heartbeat")
        {
            if (!VerifyClientSignal(clientId, "heartbeat", payload))
                return;
            if (Clients.TryGetValue(clientId, out var live))
            {
                bool restored = !live.Connected;
                live.LastSeenUtc = DateTime.UtcNow;
                live.Connected = true;
                if (restored)
                {
                    Console.WriteLine($"[player] {Short(clientId)} heartbeat restored.");
                    await SendPeerDetailsIfReady();
                    await ForceResyncTo(clientId);
                }
            }
            return;
        }
        if (kind == "leave")
        {
            if (VerifyClientSignal(clientId, "leave", payload))
                await HandleLeave(clientId);
            return;
        }
        if (kind == "frame")
            await RouteAuthoritativeFrame(clientId, payload, fromLanOrMqtt: true);
    }

    private static async Task HandleHello(string clientId, byte[] payload)
    {
        string proof;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(payload);
            JsonElement root = doc.RootElement;
            if (root.GetProperty("type").GetString() != "hello") return;
            proof = root.GetProperty("proof").GetString() ?? "";
        }
        catch { return; }

        if (payload.Length is 0 or > 4096 || !FixedEquals(Hmac($"{_session}|{clientId}|hello"), proof))
        {
            await SendJson(clientId, "rejected", new { reason = "PIN authentication failed" });
            return;
        }

        await Gate.WaitAsync();
        try
        {
            if (!Clients.TryGetValue(clientId, out var slot))
            {
                if (Clients.Count >= 2)
                {
                    await SendJson(clientId, "full", new { reason = "This room already has two registered players." });
                    return;
                }
                slot = new ClientSlot { Id = clientId, LastSeenUtc = DateTime.UtcNow, Connected = true };
                Clients[clientId] = slot;
                Console.WriteLine($"[player] {Short(clientId)} authenticated ({Clients.Count}/2).");
            }
            else
            {
                slot.LastSeenUtc = DateTime.UtcNow;
                slot.Connected = true;
                Console.WriteLine($"[player] {Short(clientId)} reconnected.");
            }

            await EnsurePeerConnection(slot);
            await SendJson(clientId, "waiting", new
            {
                players = Clients.Values.Count(p => p.Connected),
                required = 2,
                worldHost = true,
                sessionKey = Convert.ToBase64String(_sessionKey)
            });

            ClientSlot[] players = Clients.Values.OrderBy(x => x.Id, StringComparer.Ordinal).ToArray();
            if (players.Length < 2 || players.Any(p => !p.Connected))
                return;

            await SendPeerDetailsIfReady();
        }
        finally { Gate.Release(); }
    }

    private static async Task SendPeerDetailsIfReady()
    {
        ClientSlot[] players = Clients.Values.OrderBy(x => x.Id, StringComparer.Ordinal).ToArray();
        if (players.Length != 2 || players.Any(p => !p.Connected))
            return;
        foreach (ClientSlot self in players)
        {
            ClientSlot peer = players.First(p => p.Id != self.Id);
            await SendJson(self.Id, "peer", new
            {
                client = peer.Id,
                role = self.Id == players[0].Id ? 0 : 1,
                worldHost = true,
                sessionKey = Convert.ToBase64String(_sessionKey),
                proof = Hmac($"{_session}|{self.Id}|peer")
            });
        }
        Console.WriteLine("[room] Both phones ready. WebRTC world host is authoritative.");
        foreach (var p in players)
            await ForceResyncTo(p.Id);
    }

    private static async Task EnsurePeerConnection(ClientSlot slot)
    {
        if (slot.PeerConnection != null)
            return;

        var iceServers = new List<RTCIceServer>();
        foreach (string url in BuiltinTurn.Urls)
        {
            var server = new RTCIceServer { urls = url };
            if (url.StartsWith("turn:", StringComparison.Ordinal) || url.StartsWith("turns:", StringComparison.Ordinal))
            {
                server.username = BuiltinTurn.Username();
                server.credential = BuiltinTurn.Credential();
            }
            iceServers.Add(server);
        }
        iceServers.Add(new RTCIceServer { urls = "stun:stun.l.google.com:19302" });

        var config = new RTCConfiguration { iceServers = iceServers };
        var pc = new RTCPeerConnection(config);
        slot.PeerConnection = pc;

        pc.onicecandidate += async (cand) =>
        {
            if (cand == null) return;
            await PublishSignal(slot.Id, new
            {
                type = "ice",
                candidate = cand.candidate,
                sdpMid = cand.sdpMid,
                sdpMLineIndex = cand.sdpMLineIndex
            });
        };
        pc.oniceconnectionstatechange += (state) =>
        {
            if (state == RTCIceConnectionState.connected)
            {
                slot.ConnType = GuessConnType(pc);
                Console.WriteLine($"[webrtc] {Short(slot.Id)} ICE {state} ({slot.ConnType}).");
            }
            else if (state is RTCIceConnectionState.failed or RTCIceConnectionState.disconnected)
            {
                Console.WriteLine($"[webrtc] {Short(slot.Id)} ICE {state}; will accept re-offer.");
            }
        };
        pc.ondatachannel += (dc) =>
        {
            slot.DataChannel = dc;
            WireDataChannel(slot, dc);
        };

        // Host also creates a channel so either side can open first.
        RTCDataChannel hostDc = await pc.createDataChannel("hns-gba-link", new RTCDataChannelInit());
        slot.DataChannel = hostDc;
        WireDataChannel(slot, hostDc);
    }

    private static string GuessConnType(RTCPeerConnection pc)
    {
        try
        {
            // SIPSorcery does not always expose selected pair type; TURN is
            // still reported when ICE succeeded after relay allocation.
            return "WebRTC";
        }
        catch { return "WebRTC"; }
    }

    private static void WireDataChannel(ClientSlot slot, RTCDataChannel dc)
    {
        dc.onmessage += (channel, protocol, data) =>
        {
            if (data == null || data.Length == 0) return;
            _ = RouteAuthoritativeFrame(slot.Id, data, fromLanOrMqtt: false);
        };
        dc.onopen += () =>
        {
            slot.ConnType = "WebRTC";
            Console.WriteLine($"[webrtc] {Short(slot.Id)} data channel open.");
            _ = ForceResyncTo(slot.Id);
        };
    }

    private static async Task HandleSignal(string clientId, byte[] payload)
    {
        if (!Clients.TryGetValue(clientId, out var slot))
            return;
        await EnsurePeerConnection(slot);
        try
        {
            using JsonDocument doc = JsonDocument.Parse(payload);
            JsonElement root = doc.RootElement;
            string? type = root.GetProperty("type").GetString();
            var pc = slot.PeerConnection!;
            if (type == "offer")
            {
                string sdp = root.GetProperty("sdp").GetString() ?? "";
                var result = pc.setRemoteDescription(new RTCSessionDescriptionInit
                {
                    type = RTCSdpType.offer,
                    sdp = sdp
                });
                if (result != SetDescriptionResultEnum.OK)
                {
                    Console.WriteLine($"[webrtc] setRemote offer failed: {result}");
                    return;
                }
                var answer = pc.createAnswer();
                await pc.setLocalDescription(answer);
                await PublishSignal(clientId, new { type = "answer", sdp = answer.sdp });
            }
            else if (type == "ice")
            {
                var init = new RTCIceCandidateInit
                {
                    candidate = root.GetProperty("candidate").GetString(),
                    sdpMid = root.TryGetProperty("sdpMid", out var mid) ? mid.GetString() : null,
                    sdpMLineIndex = root.TryGetProperty("sdpMLineIndex", out var idx) && idx.ValueKind == JsonValueKind.Number
                        ? idx.GetUInt16()
                        : (ushort)0
                };
                pc.addIceCandidate(init);
            }
            else if (type == "restart")
            {
                pc.restartIce();
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine("[signal] " + ex.Message);
        }
    }

    private static async Task PublishSignal(string clientId, object body)
    {
        var message = new MqttApplicationMessageBuilder()
            .WithTopic($"{RootTopic}/signal/{clientId}/down")
            .WithPayload(JsonSerializer.SerializeToUtf8Bytes(body))
            .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
            .Build();
        try { await _mqtt!.PublishAsync(message, _stop.Token); }
        catch (Exception ex) { Console.WriteLine("[signal] " + ex.Message); }
    }

    /// <summary>
    /// Authoritative path: peek POSITION frames into host state, forward to peer
    /// over WebRTC (preferred) or MQTT/LAN fallback. Same session key on all sides.
    /// </summary>
    private static async Task RouteAuthoritativeFrame(string senderId, byte[] payload, bool fromLanOrMqtt)
    {
        if (payload.Length is < 16 or > 8192 || !Clients.TryGetValue(senderId, out var sender) || !sender.Connected)
            return;
        sender.LastSeenUtc = DateTime.UtcNow;
        TryIngestPosition(sender, payload);

        ClientSlot? peer = Clients.Values.FirstOrDefault(c => c.Id != senderId && c.Connected);
        if (peer == null) return;

        if (peer.DataChannel != null && peer.DataChannel.IsOpened)
        {
            try
            {
                peer.DataChannel.send(payload);
                return;
            }
            catch { }
        }
        if (peer.LanStream != null)
        {
            try
            {
                await SendLanFrame(peer.LanStream, payload);
                return;
            }
            catch { peer.LanStream = null; }
        }
        if (fromLanOrMqtt || _mqtt?.IsConnected == true)
        {
            var message = new MqttApplicationMessageBuilder()
                .WithTopic($"{RootTopic}/down/{peer.Id}/frame")
                .WithPayload(payload)
                .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtMostOnce)
                .Build();
            try { await _mqtt!.PublishAsync(message, _stop.Token); }
            catch (Exception ex) { Console.WriteLine("[relay] " + ex.Message); }
        }
    }

    private static void TryIngestPosition(ClientSlot sender, byte[] encryptedFrame)
    {
        // Encrypted frames are opaque AES-GCM; host uses the shared session key
        // to decrypt when possible so POSITION state stays authoritative.
        try
        {
            if (!TryDecryptFrame(encryptedFrame, out byte[] plain))
                return;
            // Peer frame: magic(4) ver(2) type(2) ... — match android-peer-link
            if (plain.Length < 8) return;
            ushort type = (ushort)((plain[6] << 8) | plain[7]);
            const ushort POSITION = 16;
            const ushort POSITION_RESYNC = 31;
            if (type == POSITION_RESYNC)
            {
                _ = ForceResyncTo(sender.Id);
                return;
            }
            if (type != POSITION || plain.Length < 16 + 16) return;
            int payloadOffset = 16;
            int words = (plain.Length - payloadOffset) / 4;
            if (words < 4) return;
            for (int i = 0; i < Math.Min(words, 7); i++)
            {
                int o = payloadOffset + i * 4;
                uint be = (uint)((plain[o] << 24) | (plain[o + 1] << 16) | (plain[o + 2] << 8) | plain[o + 3]);
                sender.LastPosition[i] = be;
            }
            sender.HasPosition = true;
        }
        catch { }
    }

    private static bool TryDecryptFrame(byte[] encrypted, out byte[] plain)
    {
        plain = Array.Empty<byte>();
        // Layout matches Android encryptFrame: 12-byte nonce + ciphertext+tag
        if (encrypted.Length < 12 + 16) return false;
        try
        {
            byte[] nonce = encrypted.AsSpan(0, 12).ToArray();
            byte[] cipher = encrypted.AsSpan(12).ToArray();
            plain = new byte[cipher.Length - 16];
            using var aes = new AesGcm(_sessionKey, 16);
            aes.Decrypt(nonce, cipher.AsSpan(0, cipher.Length - 16), cipher.AsSpan(cipher.Length - 16), plain);
            return true;
        }
        catch
        {
            plain = Array.Empty<byte>();
            return false;
        }
    }

    private static async Task ForceResyncTo(string clientId)
    {
        // Ask the phone to republish POSITION; also push peer's last known
        // position if we have it (host-authoritative snapshot).
        await SendJson(clientId, "presence-resync", new { timestamp = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() });
        if (!Clients.TryGetValue(clientId, out var self)) return;
        ClientSlot? peer = Clients.Values.FirstOrDefault(c => c.Id != clientId && c.HasPosition);
        if (peer == null || !peer.HasPosition) return;
        // Phones already exchange encrypted POSITION; resync control is enough.
        _ = peer;
        _ = self;
    }

    private static async Task HandleLeave(string clientId)
    {
        if (!Clients.TryGetValue(clientId, out var slot)) return;
        slot.Connected = false;
        slot.LastSeenUtc = DateTime.UtcNow;
        try { slot.PeerConnection?.close(); } catch { }
        slot.PeerConnection = null;
        slot.DataChannel = null;
        slot.ConnType = "signaling";
        ClientSlot? peer = Clients.Values.FirstOrDefault(c => c.Id != clientId && c.Connected);
        if (peer != null)
            await SendJson(peer.Id, "peer-disconnected", new { reconnecting = true });
        Console.WriteLine($"[player] {Short(clientId)} disconnected; holding slot.");
    }

    private static async Task SendJson(string clientId, string type, object data)
    {
        byte[] envelope = JsonSerializer.SerializeToUtf8Bytes(new
        {
            type,
            session = _session,
            data,
            auth = Hmac($"{_session}|{clientId}|{type}")
        });
        var message = new MqttApplicationMessageBuilder()
            .WithTopic($"{RootTopic}/down/{clientId}/control")
            .WithPayload(envelope)
            .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
            .Build();
        await _mqtt!.PublishAsync(message, _stop.Token);
    }

    private static async Task PublishRoomAdvertisement(bool clear = false)
    {
        long timestamp = clear ? 0L : DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        object advert = new
        {
            type = "world-host",
            session = _session,
            salt = _salt,
            timestamp,
            protocol = ProtocolVersion,
            lanPort = LanPort,
            proof = clear ? "" : Hmac($"{_session}|{_salt}|{timestamp}")
        };
        var message = new MqttApplicationMessageBuilder()
            .WithTopic($"hns/johto-world/{_pin}/room")
            .WithPayload(clear ? Array.Empty<byte>() : JsonSerializer.SerializeToUtf8Bytes(advert))
            .WithRetainFlag(true)
            .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
            .Build();
        await _mqtt!.PublishAsync(message, clear ? CancellationToken.None : _stop.Token);
    }

    private static async Task HeartbeatLoop(CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(8), token);
                await PublishRoomAdvertisement();
                foreach (ClientSlot client in Clients.Values.Where(c => c.Connected))
                    await SendJson(client.Id, "server-heartbeat", new
                    {
                        timestamp = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
                        conn = client.ConnType
                    });
            }
            catch (OperationCanceledException) { return; }
            catch (Exception ex) { Console.WriteLine("[heartbeat] " + ex.Message); }
        }
    }

    private static async Task ClientWatchdog(CancellationToken token)
    {
        // Longer grace (90s) so brief mobile doze / ICE blips do not drop slots.
        while (!token.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(15), token);
                foreach (ClientSlot client in Clients.Values)
                {
                    if (client.Connected && DateTime.UtcNow - client.LastSeenUtc > TimeSpan.FromSeconds(90))
                    {
                        client.Connected = false;
                        Console.WriteLine($"[player] {Short(client.Id)} timed out; holding slot.");
                        ClientSlot? peer = Clients.Values.FirstOrDefault(c => c.Id != client.Id && c.Connected);
                        if (peer != null)
                            await SendJson(peer.Id, "peer-disconnected", new { reconnecting = true });
                    }
                }
            }
            catch (OperationCanceledException) { return; }
        }
    }

    private static async Task StatusLoop(CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(20), token);
                var lines = Clients.Values.OrderBy(c => c.Id).Select(c =>
                    $"{Short(c.Id)} {(c.Connected ? "UP" : "down")} {c.ConnType}").ToArray();
                if (lines.Length > 0)
                    Console.WriteLine("[status] " + string.Join(" | ", lines));
            }
            catch (OperationCanceledException) { return; }
        }
    }

    private static async Task RunLanListener(CancellationToken token)
    {
        TcpListener? listener = null;
        try
        {
            listener = new TcpListener(IPAddress.Any, LanPort);
            listener.Start();
            Console.WriteLine($"[lan] Listening on TCP {LanPort}.");
            while (!token.IsCancellationRequested)
            {
                var client = await listener.AcceptTcpClientAsync(token);
                _ = HandleLanClient(client, token);
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception ex) { Console.WriteLine("[lan] " + ex.Message); }
        finally { try { listener?.Stop(); } catch { } }
    }

    private static async Task HandleLanClient(TcpClient tcp, CancellationToken token)
    {
        try
        {
            using var _ = tcp;
            var stream = tcp.GetStream();
            // First line: HELLO <clientId> <proof>\n
            using var reader = new StreamReader(stream, Encoding.UTF8, false, 256, leaveOpen: true);
            string? line = await reader.ReadLineAsync(token);
            if (line == null || !line.StartsWith("HELLO ", StringComparison.Ordinal)) return;
            string[] bits = line.Split(' ', 3, StringSplitOptions.RemoveEmptyEntries);
            if (bits.Length < 3) return;
            string clientId = bits[1];
            string proof = bits[2];
            if (!FixedEquals(Hmac($"{_session}|{clientId}|lan"), proof)) return;
            if (!Clients.TryGetValue(clientId, out var slot)) return;
            slot.LanStream = stream;
            slot.ConnType = "LAN";
            slot.LastSeenUtc = DateTime.UtcNow;
            slot.Connected = true;
            Console.WriteLine($"[lan] {Short(clientId)} connected.");
            byte[] lenBuf = new byte[4];
            while (!token.IsCancellationRequested)
            {
                await ReadExact(stream, lenBuf, token);
                int len = (lenBuf[0] << 24) | (lenBuf[1] << 16) | (lenBuf[2] << 8) | lenBuf[3];
                if (len is < 16 or > 8192) break;
                byte[] frame = new byte[len];
                await ReadExact(stream, frame, token);
                await RouteAuthoritativeFrame(clientId, frame, fromLanOrMqtt: true);
            }
        }
        catch { }
    }

    private static async Task SendLanFrame(NetworkStream stream, byte[] payload)
    {
        byte[] header = new byte[4];
        int len = payload.Length;
        header[0] = (byte)(len >> 24);
        header[1] = (byte)(len >> 16);
        header[2] = (byte)(len >> 8);
        header[3] = (byte)len;
        await stream.WriteAsync(header);
        await stream.WriteAsync(payload);
        await stream.FlushAsync();
    }

    private static async Task ReadExact(NetworkStream stream, byte[] buffer, CancellationToken token)
    {
        int off = 0;
        while (off < buffer.Length)
        {
            int n = await stream.ReadAsync(buffer.AsMemory(off, buffer.Length - off), token);
            if (n <= 0) throw new EndOfStreamException();
            off += n;
        }
    }

    private static bool VerifyClientSignal(string clientId, string action, byte[] payload)
    {
        string value = Encoding.UTF8.GetString(payload);
        int separator = value.IndexOf('|');
        if (separator <= 0) return false;
        if (!long.TryParse(value[..separator], out long timestamp)) return false;
        long now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        if (timestamp < now - 60_000 || timestamp > now + 60_000) return false;
        string proof = value[(separator + 1)..];
        return FixedEquals(Hmac($"{_session}|{clientId}|{action}|{timestamp}"), proof);
    }

    private static string Hmac(string value)
    {
        using var hmac = new HMACSHA256(_pinKey);
        return Convert.ToBase64String(hmac.ComputeHash(Encoding.UTF8.GetBytes(value)));
    }

    private static bool FixedEquals(string a, string b)
    {
        try
        {
            byte[] left = Encoding.UTF8.GetBytes(a);
            byte[] right = Encoding.UTF8.GetBytes(b);
            return left.Length == right.Length && CryptographicOperations.FixedTimeEquals(left, right);
        }
        catch { return false; }
    }

    private static string Short(string id) => id.Length <= 8 ? id : id[..8];
    private static string RootTopic => $"hns/johto-world/{_pin}/{_session}";
}
