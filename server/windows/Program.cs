using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using MQTTnet;
using MQTTnet.Client;
using MQTTnet.Protocol;

internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        string pin = ReadOption(args, "--pin") ?? RandomNumberGenerator.GetInt32(0, 10_000).ToString("D4");
        string broker = ReadOption(args, "--broker") ?? "broker.emqx.io";
        if (pin.Length != 4 || pin.Any(c => c < '0' || c > '9'))
        {
            Console.Error.WriteLine("PIN must contain exactly four digits.");
            return 2;
        }

        using var shutdown = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; shutdown.Cancel(); };

        Console.WriteLine("Pokémon Heart & Soul PC Co-op Server");
        Console.WriteLine("------------------------------------");
        Console.WriteLine($"PIN: {pin}");
        Console.WriteLine($"Broker: {broker}:1883");
        Console.WriteLine("Keep this window open while playing. Press Ctrl+C to stop.");
        Console.WriteLine();

        var server = new PcRelayServer(pin, broker);
        try
        {
            await server.RunAsync(shutdown.Token);
            return 0;
        }
        catch (OperationCanceledException)
        {
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"Fatal server error: {ex.Message}");
            return 1;
        }
    }

    private static string? ReadOption(string[] args, string name)
    {
        for (int i = 0; i < args.Length - 1; i++)
            if (string.Equals(args[i], name, StringComparison.OrdinalIgnoreCase))
                return args[i + 1];
        return null;
    }
}

internal sealed class PcRelayServer
{
    private const int PinKdfIterations = 500_000;
    private const int MaxFrameBytes = 8 * 1024;
    private static readonly TimeSpan ClientTimeout = TimeSpan.FromSeconds(35);
    private static readonly TimeSpan SignalClockSkew = TimeSpan.FromSeconds(30);

    private readonly string _pin;
    private readonly string _broker;
    private readonly string _session = Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant();
    private readonly string _salt = Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant();
    private readonly byte[] _pinKey;
    private readonly object _gate = new();
    private readonly List<PhoneClient> _phones = new(2);
    private IMqttClient? _mqtt;
    private DateTimeOffset _lastAdvertisement = DateTimeOffset.MinValue;
    private DateTimeOffset _lastServerHeartbeat = DateTimeOffset.MinValue;

    public PcRelayServer(string pin, string broker)
    {
        _pin = pin;
        _broker = broker;
        _pinKey = Rfc2898DeriveBytes.Pbkdf2(
            pin, Convert.FromHexString(_salt), PinKdfIterations, HashAlgorithmName.SHA256, 32);
    }

    private string RoomTopic => $"hns/johto-pc/{_pin}/room";
    private string RootTopic => $"hns/johto-pc/{_pin}/{_session}";
    private string UpFilter => $"{RootTopic}/up/+/+";

    public async Task RunAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                await ConnectAsync(cancellationToken);
                Console.WriteLine($"Connected. Session: {_session}");
                await PublishRoomAdvertisementAsync(cancellationToken);

                while (!cancellationToken.IsCancellationRequested && _mqtt?.IsConnected == true)
                {
                    await TickAsync(cancellationToken);
                    await Task.Delay(1000, cancellationToken);
                }
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                break;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Broker connection interrupted: {ex.Message}");
            }
            finally
            {
                if (_mqtt is not null)
                {
                    try
                    {
                        if (_mqtt.IsConnected)
                        {
                            await RemoveRoomAdvertisementAsync();
                            await _mqtt.DisconnectAsync(new MqttClientDisconnectOptions());
                        }
                    }
                    catch { }
                    _mqtt.Dispose();
                    _mqtt = null;
                }
            }

            if (!cancellationToken.IsCancellationRequested)
            {
                Console.WriteLine("Reconnecting to the relay broker…");
                await Task.Delay(3000, cancellationToken);
            }
        }

        try { await RemoveRoomAdvertisementAsync(); } catch { }
        Console.WriteLine("Server stopped.");
    }

    private async Task ConnectAsync(CancellationToken cancellationToken)
    {
        var factory = new MqttFactory();
        var client = factory.CreateMqttClient();
        client.ApplicationMessageReceivedAsync += e => HandleMessageAsync(e, cancellationToken);

        var options = new MqttClientOptionsBuilder()
            .WithClientId("hns-pc-" + Guid.NewGuid().ToString("N"))
            .WithTcpServer(_broker, 1883)
            .WithCleanSession()
            .WithKeepAlivePeriod(TimeSpan.FromSeconds(25))
            .Build();

        await client.ConnectAsync(options, cancellationToken);
        _mqtt = client;

        await client.SubscribeAsync(
            new MqttTopicFilterBuilder()
                .WithTopic(UpFilter)
                .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
                .Build(),
            cancellationToken);
    }

    private async Task HandleMessageAsync(MqttApplicationMessageReceivedEventArgs e, CancellationToken cancellationToken)
    {
        string[] parts = e.ApplicationMessage.Topic.Split('/');
        if (parts.Length != 7 || parts[0] != "hns" || parts[1] != "johto-pc"
            || parts[2] != _pin || parts[3] != _session || parts[4] != "up")
            return;

        string clientId = parts[5];
        string action = parts[6];
        if (clientId.Length is < 8 or > 64 || clientId.Any(c => !char.IsAsciiLetterOrDigit(c)))
            return;

        byte[] payload = e.ApplicationMessage.PayloadSegment.ToArray();
        switch (action)
        {
            case "hello":
                await HandleHelloAsync(clientId, payload, cancellationToken);
                break;
            case "heartbeat":
                HandleAuthenticatedSignal(clientId, "heartbeat", payload);
                break;
            case "leave":
                if (HandleAuthenticatedSignal(clientId, "leave", payload))
                    await RemovePhoneAsync(clientId, cancellationToken);
                break;
            case "frame":
                await ForwardFrameAsync(clientId, payload, cancellationToken);
                break;
        }
    }

    private async Task HandleHelloAsync(string clientId, byte[] payload, CancellationToken cancellationToken)
    {
        if (payload.Length is 0 or > 4096)
            return;

        string publicKey;
        string proof;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(payload);
            JsonElement root = doc.RootElement;
            if (root.GetProperty("type").GetString() != "hello")
                return;
            publicKey = root.GetProperty("pub").GetString() ?? "";
            proof = root.GetProperty("proof").GetString() ?? "";
        }
        catch
        {
            return;
        }

        if (publicKey.Length is < 40 or > 1024
            || !VerifyHmac($"{_session}|{clientId}|{publicKey}", proof))
            return;

        var phone = new PhoneClient(clientId, publicKey, proof, DateTimeOffset.UtcNow);
        List<PhoneClient> snapshot;
        bool full;
        lock (_gate)
        {
            int existing = _phones.FindIndex(p => p.Id == clientId);
            if (existing >= 0)
                _phones[existing] = phone;
            else if (_phones.Count < 2)
                _phones.Add(phone);

            full = _phones.All(p => p.Id != clientId);
            snapshot = _phones.ToList();
        }

        if (full)
        {
            await PublishControlAsync(clientId, "full", new { reason = "This co-op session already has two phones." }, cancellationToken);
            return;
        }

        Console.WriteLine($"Authenticated phone: {clientId[..Math.Min(8, clientId.Length)]}…");
        if (snapshot.Count == 1)
        {
            await PublishControlAsync(clientId, "waiting", null, cancellationToken);
            return;
        }

        await PublishPeerPairAsync(snapshot[0], snapshot[1], cancellationToken);
        Console.WriteLine("Both phones authenticated. Co-op relay is active.");
    }

    private async Task PublishPeerPairAsync(PhoneClient first, PhoneClient second, CancellationToken cancellationToken)
    {
        await PublishControlAsync(first.Id, "peer", new
        {
            client = second.Id,
            pub = second.PublicKey,
            proof = second.Proof,
            role = 0
        }, cancellationToken);

        await PublishControlAsync(second.Id, "peer", new
        {
            client = first.Id,
            pub = first.PublicKey,
            proof = first.Proof,
            role = 1
        }, cancellationToken);
    }

    private bool HandleAuthenticatedSignal(string clientId, string action, byte[] payload)
    {
        string text = Encoding.UTF8.GetString(payload);
        string[] parts = text.Split('|');
        if (parts.Length != 2 || !long.TryParse(parts[0], NumberStyles.Integer, CultureInfo.InvariantCulture, out long timestamp))
            return false;

        long now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        if (Math.Abs(now - timestamp) > SignalClockSkew.TotalMilliseconds)
            return false;

        if (!VerifyHmac($"{_session}|{clientId}|{action}|{timestamp}", parts[1]))
            return false;

        lock (_gate)
        {
            PhoneClient? phone = _phones.FirstOrDefault(p => p.Id == clientId);
            if (phone is null)
                return false;
            phone.LastSeen = DateTimeOffset.UtcNow;
        }
        return true;
    }

    private async Task ForwardFrameAsync(string senderId, byte[] payload, CancellationToken cancellationToken)
    {
        if (payload.Length is < 28 or > MaxFrameBytes)
            return;

        PhoneClient? recipient;
        lock (_gate)
        {
            if (!_phones.Any(p => p.Id == senderId))
                return;
            recipient = _phones.FirstOrDefault(p => p.Id != senderId);
        }

        if (recipient is null)
            return;

        await PublishAsync($"{RootTopic}/down/{recipient.Id}/frame", payload, retained: false, cancellationToken);
    }

    private async Task RemovePhoneAsync(string clientId, CancellationToken cancellationToken)
    {
        PhoneClient? removed;
        PhoneClient? remaining;
        lock (_gate)
        {
            removed = _phones.FirstOrDefault(p => p.Id == clientId);
            if (removed is null)
                return;
            _phones.Remove(removed);
            remaining = _phones.FirstOrDefault();
        }

        Console.WriteLine($"Phone left: {clientId[..Math.Min(8, clientId.Length)]}…");
        if (remaining is not null)
            await PublishControlAsync(remaining.Id, "peer-disconnected", null, cancellationToken);
    }

    private async Task TickAsync(CancellationToken cancellationToken)
    {
        DateTimeOffset now = DateTimeOffset.UtcNow;
        List<string> expired;
        List<PhoneClient> phones;
        lock (_gate)
        {
            expired = _phones.Where(p => now - p.LastSeen > ClientTimeout).Select(p => p.Id).ToList();
            phones = _phones.ToList();
        }

        foreach (string id in expired)
            await RemovePhoneAsync(id, cancellationToken);

        if (now - _lastAdvertisement >= TimeSpan.FromSeconds(10))
            await PublishRoomAdvertisementAsync(cancellationToken);

        if (now - _lastServerHeartbeat >= TimeSpan.FromSeconds(10))
        {
            foreach (PhoneClient phone in phones)
                await PublishControlAsync(phone.Id, "server-heartbeat", null, cancellationToken);
            _lastServerHeartbeat = now;
        }
    }

    private async Task PublishRoomAdvertisementAsync(CancellationToken cancellationToken)
    {
        long timestamp = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        string proof = HmacText($"{_session}|{_salt}|{timestamp}");
        byte[] payload = JsonSerializer.SerializeToUtf8Bytes(new
        {
            type = "pc-room",
            session = _session,
            salt = _salt,
            timestamp,
            proof
        });

        await PublishAsync(RoomTopic, payload, retained: true, cancellationToken);
        _lastAdvertisement = DateTimeOffset.UtcNow;
    }

    private async Task PublishControlAsync(string clientId, string type, object? data, CancellationToken cancellationToken)
    {
        var envelope = new Dictionary<string, object?>
        {
            ["type"] = type,
            ["session"] = _session
        };
        if (data is not null)
            envelope["data"] = data;

        await PublishAsync($"{RootTopic}/down/{clientId}/control",
            JsonSerializer.SerializeToUtf8Bytes(envelope), retained: false, cancellationToken);
    }

    private async Task PublishAsync(string topic, byte[] payload, bool retained, CancellationToken cancellationToken)
    {
        IMqttClient? client = _mqtt;
        if (client is null || !client.IsConnected)
            return;

        var message = new MqttApplicationMessageBuilder()
            .WithTopic(topic)
            .WithPayload(payload)
            .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
            .WithRetainFlag(retained)
            .Build();

        await client.PublishAsync(message, cancellationToken);
    }

    private bool VerifyHmac(string text, string supplied)
    {
        if (string.IsNullOrWhiteSpace(supplied))
            return false;
        byte[] expected = HMACSHA256.HashData(_pinKey, Encoding.UTF8.GetBytes(text));
        byte[] actual;
        try { actual = Convert.FromBase64String(supplied); }
        catch { return false; }
        return actual.Length == expected.Length && CryptographicOperations.FixedTimeEquals(expected, actual);
    }

    private string HmacText(string text) =>
        Convert.ToBase64String(HMACSHA256.HashData(_pinKey, Encoding.UTF8.GetBytes(text)));

    private async Task RemoveRoomAdvertisementAsync()
    {
        if (_mqtt?.IsConnected != true)
            return;
        await PublishAsync(RoomTopic, Array.Empty<byte>(), retained: true, CancellationToken.None);
    }

    private sealed class PhoneClient(string id, string publicKey, string proof, DateTimeOffset lastSeen)
    {
        public string Id { get; } = id;
        public string PublicKey { get; } = publicKey;
        public string Proof { get; } = proof;
        public DateTimeOffset LastSeen { get; set; } = lastSeen;
    }
}
