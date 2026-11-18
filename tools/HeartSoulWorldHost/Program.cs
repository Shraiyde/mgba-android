using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;

namespace HeartSoulWorldHost;

/// <summary>
/// ZeroTier Minecraft-style world host: phones one-tap join with baked credentials.
/// No MQTT, WebRTC, TURN, UPnP, PIN entry, or relay. Shared secret never logged.
/// </summary>
internal sealed class ClientSlot
{
    public required string Id { get; init; }
    public DateTime LastSeenUtc { get; set; }
    public bool Connected { get; set; }
    public NetworkStream? Stream { get; set; }
    public readonly object StreamLock = new();
    public uint[] LastPosition { get; set; } = new uint[8];
    public int PositionWords { get; set; }
    public bool HasPosition { get; set; }
    public bool HasClaimSeq { get; set; }
    public byte LastClaimSeq { get; set; }
    public int Role { get; set; } = -1;
}

internal static class Program
{
    private const int ProtocolVersion = 10;
    private const uint PeerMagic = 0x4D504C4Bu;
    private const ushort PeerVersion = 1;
    private const int HostSenderId = 2;
    private const ushort MsgPosition = 16;
    private const ushort MsgWorldFlag = 17;
    private const ushort MsgSioMulti = 23;
    private const ushort MsgWorldVar = 24;
    private const ushort MsgWorldReady = 25;
    private const ushort MsgWorldConfig = 26;
    private const ushort MsgWorldConfigAck = 27;
    private const ushort MsgSioResync = 28;
    private const ushort MsgWorldConfigBlob = 29;
    private const ushort MsgWorldResync = 30;
    private const ushort MsgPositionResync = 31;
    private static readonly TimeSpan DropGrace = TimeSpan.FromSeconds(10);

    private static readonly ConcurrentDictionary<string, ClientSlot> Clients = new();
    private static readonly SemaphoreSlim Gate = new(1, 1);
    private static byte[] _sessionKey = [];
    private static long _hostSendCounter = 1;
    private static int _hostFrameSeq = 1;
    private static CancellationTokenSource _stop = new();
    private static int _listenPort;

    private static async Task Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;
        _listenPort = BuiltinPcHostConfig.Port();

        if (args.Length >= 1 && args[0].Equals("--add-firewall", StringComparison.OrdinalIgnoreCase))
        {
            int port = args.Length >= 2 && int.TryParse(args[1], out int p) ? p : _listenPort;
            Environment.Exit(AddFirewallRule(port) ? 0 : 1);
            return;
        }

        _sessionKey = BuiltinPcHostConfig.SessionKey();
        EnsureFirewallRule(_listenPort);

        string[] zt = CollectAdapterIps("ZeroTier");

        string bakedHost = BuiltinPcHostConfig.Host();
        Console.WriteLine("HEART & SOUL — WORLD HOST");
        Console.WriteLine("=========================");
        Console.WriteLine("Mode:       ZeroTier direct TCP (baked credentials; no PIN / MQTT / TURN)");
        Console.WriteLine("Listen:     TCP *:" + _listenPort + " (all interfaces)");
        Console.WriteLine("Phones dial: " + bakedHost + ":" + _listenPort + " (baked into APK)");
        Console.WriteLine("Heartbeat:  1s  |  phone soft-resume timeout: 10s");
        Console.WriteLine("ZeroTier:  " + (zt.Length > 0 ? string.Join(", ", zt) : "(not detected — start ZeroTier or phones cannot reach this PC)"));
        Console.WriteLine("Protocol:  v" + ProtocolVersion);
        Console.WriteLine();
        Console.WriteLine("Phones: Co-op → Join PC Host (one tap — credentials are built into the APK).");
        Console.WriteLine("Both phones must be on Stephen's ZeroTier network.");
        Console.WriteLine("Every TCP accept / HELLO reject is logged below (no secrets).");
        Console.WriteLine("Press Ctrl+C to stop.");
        Console.WriteLine();

        Console.CancelKeyPress += (_, e) => { e.Cancel = true; _stop.Cancel(); };

        _ = RunGameListener(_stop.Token);
        _ = HostHeartbeatLoop(_stop.Token);
        _ = ClientWatchdog(_stop.Token);
        _ = StatusLoop(_stop.Token);
        _ = WorldStreamLoop(_stop.Token);

        try { await Task.Delay(Timeout.Infinite, _stop.Token); }
        catch (OperationCanceledException) { }

        Console.WriteLine("World host stopped.");
    }

    private static string[] CollectAdapterIps(string needle)
    {
        var list = new List<string>();
        try
        {
            foreach (NetworkInterface nic in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (nic.OperationalStatus != OperationalStatus.Up)
                    continue;
                string label = nic.Name + " " + nic.Description;
                if (label.IndexOf(needle, StringComparison.OrdinalIgnoreCase) < 0)
                    continue;
                foreach (UnicastIPAddressInformation uni in nic.GetIPProperties().UnicastAddresses)
                {
                    if (uni.Address.AddressFamily != AddressFamily.InterNetwork)
                        continue;
                    if (IPAddress.IsLoopback(uni.Address))
                        continue;
                    list.Add(uni.Address.ToString());
                }
            }
        }
        catch { }
        return list.Distinct().ToArray();
    }

    private static void EnsureFirewallRule(int port)
    {
        if (AddFirewallRule(port))
        {
            Console.WriteLine("[firewall] Inbound TCP " + port + " allowed.");
            return;
        }

        Console.WriteLine("[firewall] Need Administrator once to open TCP " + port + ".");
        Console.WriteLine("[firewall] Approve the Windows UAC prompt if it appears…");
        try
        {
            string? exe = Environment.ProcessPath;
            if (string.IsNullOrEmpty(exe))
                exe = "HeartSoulWorldHost.exe";
            var psi = new ProcessStartInfo
            {
                FileName = exe,
                Arguments = "--add-firewall " + port,
                UseShellExecute = true,
                Verb = "runas"
            };
            using Process? elevated = Process.Start(psi);
            if (elevated == null)
            {
                Console.WriteLine("[firewall] UAC cancelled. Add an inbound allow for TCP " + port + " manually.");
                return;
            }
            elevated.WaitForExit(20000);
            Console.WriteLine(elevated.ExitCode == 0
                ? "[firewall] Inbound TCP " + port + " allowed (elevated)."
                : "[firewall] Elevated add failed. Allow TCP " + port + " in Windows Firewall manually.");
        }
        catch (Exception ex)
        {
            Console.WriteLine("[firewall] " + ex.Message);
            Console.WriteLine("[firewall] Allow TCP " + port + " inbound in Windows Defender Firewall if phones cannot connect.");
        }
    }

    private static bool AddFirewallRule(int port)
    {
        try
        {
            string name = "HeartSoulWorldHost TCP " + port;
            RunNetsh($"advfirewall firewall delete rule name=\"{name}\"");
            return RunNetsh(
                $"advfirewall firewall add rule name=\"{name}\" dir=in action=allow protocol=TCP localport={port} profile=any") == 0;
        }
        catch
        {
            return false;
        }
    }

    private static int RunNetsh(string args)
    {
        var psi = new ProcessStartInfo
        {
            FileName = "netsh",
            Arguments = args,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
        };
        using var p = Process.Start(psi);
        if (p == null) return -1;
        p.WaitForExit(8000);
        return p.ExitCode;
    }

    private static async Task RunGameListener(CancellationToken token)
    {
        TcpListener? listener = null;
        try
        {
            listener = new TcpListener(IPAddress.Any, _listenPort);
            listener.Start();
            Console.WriteLine("[tcp] Listening on all interfaces, port " + _listenPort + ".");
            while (!token.IsCancellationRequested)
            {
                TcpClient client = await listener.AcceptTcpClientAsync(token);
                _ = HandleGameClient(client, token);
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception ex) { Console.WriteLine("[tcp] " + ex.Message); }
        finally { try { listener?.Stop(); } catch { } }
    }

    private static async Task HandleGameClient(TcpClient tcp, CancellationToken token)
    {
        string? clientId = null;
        NetworkStream? ownedStream = null;
        ClientSlot? slot = null;
        string remote = RemoteEndpoint(tcp);
        Console.WriteLine($"[tcp] accept from {remote}");
        try
        {
            tcp.NoDelay = true;
            tcp.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.KeepAlive, true);
            NetworkStream stream = tcp.GetStream();
            ownedStream = stream;
            string? line = await ReadLineExact(stream, token);
            if (line == null)
            {
                Console.WriteLine($"[tcp] {remote} closed before HELLO");
                return;
            }
            if (!line.StartsWith("HELLO ", StringComparison.Ordinal))
            {
                Console.WriteLine($"[tcp] {remote} rejected: expected HELLO, got {SanitizeHandshakePreview(line)}");
                return;
            }
            string[] bits = line.Split(' ', 3, StringSplitOptions.RemoveEmptyEntries);
            if (bits.Length < 3)
            {
                Console.WriteLine($"[tcp] {remote} rejected: HELLO missing clientId/proof");
                return;
            }
            clientId = bits[1];
            string proof = bits[2];
            if (clientId.Length is < 8 or > 64
                || clientId.Any(c => !char.IsAsciiLetterOrDigit(c) && c != '-'))
            {
                Console.WriteLine($"[tcp] {remote} rejected: invalid clientId shape");
                return;
            }
            if (!FixedEquals(BuiltinPcHostConfig.HelloProof(clientId), proof))
            {
                Console.WriteLine($"[tcp] {remote} id={Short(clientId)} rejected: bad HELLO proof");
                return;
            }

            NetworkStream? staleStream = null;
            await Gate.WaitAsync(token);
            try
            {
                if (!Clients.TryGetValue(clientId, out slot))
                {
                    if (Clients.Count >= 2)
                    {
                        Console.WriteLine($"[tcp] {remote} id={Short(clientId)} rejected: room full (2/2)");
                        await stream.WriteAsync(Encoding.UTF8.GetBytes("FULL\n"), token);
                        return;
                    }
                    slot = new ClientSlot
                    {
                        Id = clientId,
                        LastSeenUtc = DateTime.UtcNow,
                        Connected = true,
                        Role = Clients.Count
                    };
                    Clients[clientId] = slot;
                    Console.WriteLine($"[player] {Short(clientId)} joined from {remote} ({Clients.Count}/2) role={slot.Role}");
                }
                else
                {
                    // Same player resumed: replace the stale socket — never keep two.
                    lock (slot.StreamLock)
                    {
                        staleStream = slot.Stream;
                        slot.Stream = null;
                    }
                    slot.LastSeenUtc = DateTime.UtcNow;
                    slot.Connected = true;
                    // Soft resume may restart claimSeq at the phone; accept the
                    // next claim instead of treating it as a late duplicate.
                    slot.HasClaimSeq = false;
                    Console.WriteLine($"[player] {Short(clientId)} resumed from {remote} (replaced stale socket)");
                }
            }
            finally { Gate.Release(); }

            if (staleStream != null)
            {
                try { staleStream.Close(); } catch { }
            }

            lock (slot.StreamLock)
                slot.Stream = stream;

            string ready = $"READY {slot.Role} {Clients.Count}\n";
            await stream.WriteAsync(Encoding.UTF8.GetBytes(ready), token);
            await stream.FlushAsync(token);
            Console.WriteLine($"[tcp] {Short(clientId)} READY role={slot.Role} players={Clients.Count}");
            await ForceResyncTo(clientId);

            byte[] lenBuf = new byte[4];
            while (!token.IsCancellationRequested)
            {
                await ReadExact(stream, lenBuf, token);
                int len = (lenBuf[0] << 24) | (lenBuf[1] << 16) | (lenBuf[2] << 8) | lenBuf[3];
                if (len is < 16 or > 8192)
                {
                    Console.WriteLine($"[tcp] {Short(clientId)} bad frame length {len}; closing");
                    break;
                }
                byte[] frame = new byte[len];
                await ReadExact(stream, frame, token);
                slot.LastSeenUtc = DateTime.UtcNow;
                slot.Connected = true;
                await RouteAuthoritativeFrame(clientId, frame);
            }
        }
        catch (Exception ex)
        {
            string reason = string.IsNullOrEmpty(ex.Message) ? ex.GetType().Name : ex.Message;
            Console.WriteLine($"[tcp] {(clientId != null ? Short(clientId) : remote)} error: {reason}");
        }
        finally
        {
            if (slot != null && ownedStream != null)
            {
                lock (slot.StreamLock)
                {
                    if (ReferenceEquals(slot.Stream, ownedStream))
                        slot.Stream = null;
                }
            }
            if (clientId != null)
                Console.WriteLine($"[tcp] {Short(clientId)} link down; holding slot.");
            else
                Console.WriteLine($"[tcp] {remote} closed without a session.");
            try { tcp.Dispose(); } catch { }
        }
    }

    private static string RemoteEndpoint(TcpClient tcp)
    {
        try
        {
            if (tcp.Client.RemoteEndPoint is IPEndPoint ep)
                return ep.Address + ":" + ep.Port;
        }
        catch { }
        return "unknown";
    }

    private static string SanitizeHandshakePreview(string line)
    {
        // Never echo proofs/secrets — only the first token and length.
        string[] bits = line.Split(' ', 2, StringSplitOptions.RemoveEmptyEntries);
        string head = bits.Length > 0 ? bits[0] : "(empty)";
        if (head.Length > 24)
            head = head[..24] + "…";
        return head + " (len=" + line.Length + ")";
    }

    private static async Task RouteAuthoritativeFrame(string senderId, byte[] encryptedClaim)
    {
        if (encryptedClaim.Length is < 16 or > 8192
            || !Clients.TryGetValue(senderId, out var sender)
            || !sender.Connected)
            return;

        if (!TryDecryptFrame(encryptedClaim, out byte[] plain) || plain.Length < 16)
            return;
        if (!TryParsePeerHeader(plain, out ushort type, out byte[] payload))
            return;

        ClientSlot? peer = Clients.Values.FirstOrDefault(c => c.Id != senderId && c.Connected);

        if (type == MsgPositionResync)
        {
            await ForceResyncTo(senderId);
            if (peer != null)
                await ForceResyncTo(peer.Id);
            return;
        }

        if (type == MsgPosition)
        {
            if (payload.Length < 16) return;
            int words = Math.Min(payload.Length / 4, 8);
            uint[] claim = new uint[8];
            for (int i = 0; i < words; i++)
                claim[i] = BinaryPrimitives.ReadUInt32BigEndian(payload.AsSpan(i * 4, 4));

            // Claim seq lives in the high byte of avatarState (word 7). Drop
            // exact duplicates. A large forward jump (delta >= 128) is treated
            // as a resync — rejecting it permanently locked POSITION until the
            // next TCP soft-resume cleared HasClaimSeq.
            if (words >= 8)
            {
                byte claimSeq = (byte)(claim[7] >> 24);
                if (sender.HasClaimSeq)
                {
                    byte delta = (byte)(claimSeq - sender.LastClaimSeq);
                    if (delta == 0)
                        return;
                }
                sender.LastClaimSeq = claimSeq;
                sender.HasClaimSeq = true;
            }

            for (int i = 0; i < words; i++)
                sender.LastPosition[i] = claim[i];
            sender.PositionWords = Math.Max(words, 4);
            sender.HasPosition = true;
            if (peer == null) return;
            await StreamPositionTo(peer, sender);
            return;
        }

        if (peer == null) return;
        if (type is MsgWorldFlag or MsgWorldVar or MsgWorldReady or MsgWorldConfig
            or MsgWorldConfigAck or MsgWorldConfigBlob or MsgWorldResync
            or MsgSioMulti or MsgSioResync)
        {
            await DeliverHostFrame(peer, BuildPeerFrame(type, payload));
        }
    }

    private static async Task StreamPositionTo(ClientSlot dest, ClientSlot source)
    {
        if (!source.HasPosition) return;
        int words = Math.Clamp(source.PositionWords, 4, 8);
        byte[] payload = new byte[words * 4];
        for (int i = 0; i < words; i++)
            BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(i * 4, 4), source.LastPosition[i]);
        await DeliverHostFrame(dest, BuildPeerFrame(MsgPosition, payload));
    }

    private static async Task WorldStreamLoop(CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(TimeSpan.FromMilliseconds(200), token);
                ClientSlot[] live = Clients.Values
                    .Where(c => c.Connected && HasStream(c))
                    .OrderBy(c => c.Id, StringComparer.Ordinal)
                    .ToArray();
                if (live.Length != 2) continue;
                if (live[0].HasPosition)
                    await StreamPositionTo(live[1], live[0]);
                if (live[1].HasPosition)
                    await StreamPositionTo(live[0], live[1]);
            }
            catch (OperationCanceledException) { return; }
            catch (Exception ex) { Console.WriteLine("[world] " + ex.Message); }
        }
    }

    private static async Task HostHeartbeatLoop(CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(1), token);
                byte[] beat = BuildPeerFrame(MsgWorldReady, Array.Empty<byte>());
                foreach (ClientSlot client in Clients.Values.Where(c => c.Connected && HasStream(c)))
                    await DeliverHostFrame(client, beat);
            }
            catch (OperationCanceledException) { return; }
            catch { }
        }
    }

    private static async Task ClientWatchdog(CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(2), token);
                foreach (ClientSlot client in Clients.Values)
                {
                    bool tcpUp = HasStream(client);
                    if (client.Connected && !tcpUp
                        && DateTime.UtcNow - client.LastSeenUtc > DropGrace)
                    {
                        client.Connected = false;
                        Console.WriteLine($"[player] {Short(client.Id)} timed out; holding slot for resume.");
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
                    $"{Short(c.Id)} role={c.Role} {(c.Connected ? "UP" : "down")} tcp={(HasStream(c) ? "yes" : "no")}"
                ).ToArray();
                if (lines.Length > 0)
                    Console.WriteLine("[status] " + string.Join(" | ", lines));
            }
            catch (OperationCanceledException) { return; }
        }
    }

    private static bool HasStream(ClientSlot c)
    {
        lock (c.StreamLock)
            return c.Stream != null;
    }

    private static bool TryParsePeerHeader(byte[] plain, out ushort type, out byte[] payload)
    {
        type = 0;
        payload = Array.Empty<byte>();
        if (plain.Length < 16) return false;
        uint magic = BinaryPrimitives.ReadUInt32BigEndian(plain.AsSpan(0, 4));
        ushort ver = BinaryPrimitives.ReadUInt16BigEndian(plain.AsSpan(4, 2));
        type = BinaryPrimitives.ReadUInt16BigEndian(plain.AsSpan(6, 2));
        uint length = BinaryPrimitives.ReadUInt32BigEndian(plain.AsSpan(8, 4));
        if (magic != PeerMagic || ver != PeerVersion || length > 4096 || plain.Length != 16 + length)
            return false;
        payload = plain.AsSpan(16, (int)length).ToArray();
        return true;
    }

    private static byte[] BuildPeerFrame(ushort type, byte[] payload)
    {
        byte[] frame = new byte[16 + payload.Length];
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(0, 4), PeerMagic);
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(4, 2), PeerVersion);
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(6, 2), type);
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(8, 4), (uint)payload.Length);
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(12, 4), (uint)Interlocked.Increment(ref _hostFrameSeq));
        if (payload.Length > 0)
            payload.CopyTo(frame, 16);
        return frame;
    }

    private static byte[] EncryptHostFrame(byte[] plain)
    {
        long counter = Interlocked.Increment(ref _hostSendCounter);
        byte[] nonce = new byte[12];
        BinaryPrimitives.WriteInt32BigEndian(nonce.AsSpan(0, 4), HostSenderId);
        BinaryPrimitives.WriteInt64BigEndian(nonce.AsSpan(4, 8), counter);
        byte[] cipher = new byte[plain.Length];
        byte[] tag = new byte[16];
        using var aes = new AesGcm(_sessionKey, 16);
        aes.Encrypt(nonce, plain, cipher, tag);
        byte[] output = new byte[12 + cipher.Length + tag.Length];
        nonce.CopyTo(output, 0);
        cipher.CopyTo(output, 12);
        tag.CopyTo(output, 12 + cipher.Length);
        return output;
    }

    private static async Task DeliverHostFrame(ClientSlot dest, byte[] plainFrame)
    {
        NetworkStream? stream;
        lock (dest.StreamLock)
            stream = dest.Stream;
        if (stream == null)
            return;
        try
        {
            byte[] encrypted = EncryptHostFrame(plainFrame);
            await SendTcpFrame(stream, encrypted);
        }
        catch
        {
            lock (dest.StreamLock)
            {
                if (ReferenceEquals(dest.Stream, stream))
                    dest.Stream = null;
            }
        }
    }

    private static bool TryDecryptFrame(byte[] encrypted, out byte[] plain)
    {
        plain = Array.Empty<byte>();
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
        if (!Clients.TryGetValue(clientId, out var self)) return;
        await DeliverHostFrame(self, BuildPeerFrame(MsgWorldResync, Array.Empty<byte>()));
        ClientSlot? peer = Clients.Values.FirstOrDefault(c => c.Id != clientId && c.HasPosition);
        if (peer != null)
            await StreamPositionTo(self, peer);
    }

    private static async Task SendTcpFrame(NetworkStream stream, byte[] payload)
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

    private static async Task<string?> ReadLineExact(NetworkStream stream, CancellationToken token)
    {
        var buf = new MemoryStream(64);
        byte[] one = new byte[1];
        while (true)
        {
            int n = await stream.ReadAsync(one.AsMemory(0, 1), token);
            if (n <= 0)
                return buf.Length == 0 ? null : Encoding.UTF8.GetString(buf.ToArray());
            if (one[0] == (byte)'\n')
                break;
            if (one[0] != (byte)'\r')
                buf.WriteByte(one[0]);
            if (buf.Length > 256)
                throw new InvalidDataException("handshake line too long");
        }
        return Encoding.UTF8.GetString(buf.ToArray());
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
}
