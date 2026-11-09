using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using MQTTnet;
using MQTTnet.Client;
using MQTTnet.Protocol;

namespace HeartSoulCoopServer;

internal sealed class ClientSlot
{
    public required string Id { get; init; }
    public required string PublicKey { get; set; }
    public DateTime LastSeenUtc { get; set; }
    public bool Connected { get; set; }
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
    private static readonly ConcurrentDictionary<string, ClientSlot> Clients = new();
    private static readonly SemaphoreSlim Gate = new(1, 1);
    private static IMqttClient? _mqtt;
    private static string _pin = "";
    private static string _session = "";
    private static string _salt = "";
    private static byte[] _pinKey = [];
    private static CancellationTokenSource _stop = new();

    private static async Task Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;
        string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HeartSoulCoopServer");
        Directory.CreateDirectory(root);
        string configPath = Path.Combine(root, "server.json");
        bool resetPin = args.Any(a => a.Equals("--new-pin", StringComparison.OrdinalIgnoreCase));

        RoomConfig config = File.Exists(configPath)
            ? JsonSerializer.Deserialize<RoomConfig>(await File.ReadAllTextAsync(configPath)) ?? new RoomConfig()
            : new RoomConfig();

        if (resetPin || string.IsNullOrWhiteSpace(config.Pin) || config.Pin.Length != 4 || config.Pin.Any(c => !char.IsAsciiDigit(c)))
        {
            config.Pin = RandomNumberGenerator.GetInt32(0, 10_000).ToString("D4");
            await File.WriteAllTextAsync(configPath, JsonSerializer.Serialize(config, new JsonSerializerOptions { WriteIndented = true }));
        }

        _pin = config.Pin!;
        _session = Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant();
        _salt = Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant();
        _pinKey = Rfc2898DeriveBytes.Pbkdf2(_pin, Convert.FromHexString(_salt), Pbkdf2Iterations, HashAlgorithmName.SHA256, 32);

        Console.WriteLine("HEART & SOUL — CO-OP PC SERVER");
        Console.WriteLine("--------------------------------");
        Console.WriteLine("JOIN PIN: " + _pin);
        Console.WriteLine("Keep this window open while playing.");
        Console.WriteLine("Both Android players enter this PIN in the PC Server connection screen.");
        Console.WriteLine("Internet relay: outbound MQTT over TLS (TCP 8883); no router port-forward required.");
        Console.WriteLine("Persistent PIN file: " + configPath);
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

        await ReconnectLoop();
        _ = HeartbeatLoop(_stop.Token);
        _ = ClientWatchdog(_stop.Token);

        try { await Task.Delay(Timeout.Infinite, _stop.Token); }
        catch (OperationCanceledException) { }

        try
        {
            await PublishRoomAdvertisement(clear: true);
            if (_mqtt.IsConnected)
                await _mqtt.DisconnectAsync();
        }
        catch { }
        Console.WriteLine("Server stopped.");
    }

    private static async Task ConnectAndAdvertise()
    {
        var options = new MqttClientOptionsBuilder()
            .WithTcpServer(BrokerHost, BrokerPort)
            .WithTlsOptions(tls => tls.UseTls())
            .WithClientId("hns-pc-" + Guid.NewGuid().ToString("N"))
            .WithCleanSession()
            .WithKeepAlivePeriod(TimeSpan.FromSeconds(25))
            .Build();

        await _mqtt!.ConnectAsync(options, _stop.Token);
        await _mqtt.SubscribeAsync(new MqttTopicFilterBuilder()
            .WithTopic($"{RootTopic}/up/+/+")
            .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
            .Build(), _stop.Token);
        await PublishRoomAdvertisement();
        Console.WriteLine("[network] Connected to relay broker. Waiting for both players...");
    }

    private static async Task ReconnectLoop()
    {
        while (!_stop.IsCancellationRequested && !_mqtt!.IsConnected)
        {
            try
            {
                await Task.Delay(3000, _stop.Token);
                await ConnectAndAdvertise();
                Console.WriteLine("[network] Room restored.");
                return;
            }
            catch (OperationCanceledException) { return; }
            catch (Exception ex) { Console.WriteLine("[network] " + ex.Message); }
        }
    }

    private static async Task OnMessage(MqttApplicationMessageReceivedEventArgs e)
    {
        string[] parts = e.ApplicationMessage.Topic.Split('/');
        if (parts.Length != 7 || parts[0] != "hns" || parts[1] != "johto-pc" ||
            parts[2] != _pin || parts[3] != _session || parts[4] != "up")
            return;

        string clientId = parts[5];
        string kind = parts[6];
        if (clientId.Length is < 8 or > 64 || clientId.Any(c => !char.IsAsciiLetterOrDigit(c) && c != '-'))
            return;

        byte[] payload = e.ApplicationMessage.PayloadSegment.ToArray();
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
                    await SendPeerDetailsIfReady();
            }
            return;
        }
        if (kind == "leave")
        {
            if (VerifyClientSignal(clientId, "leave", payload))
                await HandleLeave(clientId);
            return;
        }
        if (kind == "frame") await RouteFrame(clientId, payload);
    }

    private static async Task HandleHello(string clientId, byte[] payload)
    {
        string publicKey;
        string proof;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(payload);
            JsonElement root = doc.RootElement;
            if (root.GetProperty("type").GetString() != "hello") return;
            publicKey = root.GetProperty("pub").GetString() ?? "";
            proof = root.GetProperty("proof").GetString() ?? "";
        }
        catch { return; }

        if (payload.Length is 0 or > 4096 || publicKey.Length is < 40 or > 1024
            || !FixedEquals(Hmac($"{_session}|{clientId}|{publicKey}"), proof))
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
                slot = new ClientSlot { Id = clientId, PublicKey = publicKey, LastSeenUtc = DateTime.UtcNow, Connected = true };
                Clients[clientId] = slot;
                Console.WriteLine($"[player] Client {Clients.Count}/2 authenticated.");
            }
            else
            {
                slot.PublicKey = publicKey;
                slot.LastSeenUtc = DateTime.UtcNow;
                slot.Connected = true;
                Console.WriteLine("[player] Client reconnected.");
            }

            ClientSlot[] players = Clients.Values.OrderBy(x => x.Id, StringComparer.Ordinal).ToArray();
            if (players.Length < 2 || players.Any(p => !p.Connected))
            {
                await SendJson(clientId, "waiting", new { players = players.Count(p => p.Connected), required = 2 });
                return;
            }

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
                pub = peer.PublicKey,
                proof = Hmac($"{_session}|{peer.Id}|{peer.PublicKey}"),
                role = self.Id == players[0].Id ? 0 : 1
            });
        }
        Console.WriteLine("[room] Both players authenticated. Relay is ready.");
    }

    private static async Task RouteFrame(string senderId, byte[] payload)
    {
        if (payload.Length is < 28 or > 8192 || !Clients.TryGetValue(senderId, out var sender) || !sender.Connected)
            return;
        ClientSlot? peer = Clients.Values.FirstOrDefault(c => c.Id != senderId && c.Connected);
        if (peer == null) return;

        var message = new MqttApplicationMessageBuilder()
            .WithTopic($"{RootTopic}/down/{peer.Id}/frame")
            .WithPayload(payload)
            .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
            .Build();
        try { await _mqtt!.PublishAsync(message, _stop.Token); }
        catch (Exception ex) { Console.WriteLine("[relay] " + ex.Message); }
    }

    private static async Task HandleLeave(string clientId)
    {
        if (!Clients.TryGetValue(clientId, out var slot)) return;
        slot.Connected = false;
        slot.LastSeenUtc = DateTime.UtcNow;
        ClientSlot? peer = Clients.Values.FirstOrDefault(c => c.Id != clientId && c.Connected);
        if (peer != null)
            await SendJson(peer.Id, "peer-disconnected", new { reconnecting = true });
        Console.WriteLine("[player] A player disconnected; holding their slot for reconnect.");
    }

    private static async Task SendJson(string clientId, string type, object data)
    {
        byte[] envelope = JsonSerializer.SerializeToUtf8Bytes(new { type, session = _session, data });
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
            type = "pc-room",
            session = _session,
            salt = _salt,
            timestamp,
            protocol = 1,
            proof = clear ? "" : Hmac($"{_session}|{_salt}|{timestamp}")
        };
        var message = new MqttApplicationMessageBuilder()
            .WithTopic($"hns/johto-pc/{_pin}/room")
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
                await Task.Delay(TimeSpan.FromSeconds(10), token);
                await PublishRoomAdvertisement();
                foreach (ClientSlot client in Clients.Values.Where(c => c.Connected))
                    await SendJson(client.Id, "server-heartbeat", new { timestamp = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() });
            }
            catch (OperationCanceledException) { return; }
            catch (Exception ex) { Console.WriteLine("[heartbeat] " + ex.Message); }
        }
    }

    private static async Task ClientWatchdog(CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(15), token);
                foreach (ClientSlot client in Clients.Values)
                {
                    if (client.Connected && DateTime.UtcNow - client.LastSeenUtc > TimeSpan.FromSeconds(45))
                    {
                        client.Connected = false;
                        Console.WriteLine("[player] Client heartbeat lost; holding slot for reconnect.");
                        ClientSlot? peer = Clients.Values.FirstOrDefault(c => c.Id != client.Id && c.Connected);
                        if (peer != null) await SendJson(peer.Id, "peer-disconnected", new { reconnecting = true });
                    }
                }
            }
            catch (OperationCanceledException) { return; }
        }
    }

    private static bool VerifyClientSignal(string clientId, string action, byte[] payload)
    {
        string value = Encoding.UTF8.GetString(payload);
        int separator = value.IndexOf('|');
        if (separator <= 0)
            return false;

        if (!long.TryParse(value[..separator], out long timestamp))
            return false;

        long now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        if (timestamp < now - 30_000 || timestamp > now + 30_000)
            return false;

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

    private static string RootTopic => $"hns/johto-pc/{_pin}/{_session}";
}
