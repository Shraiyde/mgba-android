using System.Security.Cryptography;
using System.Text;

namespace HeartSoulWorldHost;

/// <summary>
/// Baked ZeroTier host endpoint + join key. XOR-obfuscated; never log decoded values.
/// </summary>
internal static class BuiltinPcHostConfig
{
    // Obfuscation key (not the join secret).
    private static readonly byte[] K = {
        0xA7, 0x3C, 0x91, 0x5E, 0x28, 0xF4, 0x6B, 0xD0, 0x19, 0xC2, 0x7F, 0x44,
        0xE8, 0x33, 0xB5, 0x6A, 0x0D, 0x9F, 0x52, 0xC8, 0x17, 0xE1, 0x4A, 0x76
    };

    private static readonly byte[] HostEnc = {
        0x96, 0x0C, 0xBF, 0x6F, 0x1B, 0xC3, 0x45, 0xE1, 0x2E, 0xFA, 0x51, 0x75, 0xDE, 0x02
    };
    private static readonly byte[] PortEnc = { 0x95, 0x0B, 0xA6, 0x69, 0x18 };
    private static readonly byte[] JoinEnc = {
        0xEF, 0x4F, 0xCB, 0x2A, 0x11, 0x9F, 0x3A, 0xBD, 0x2B, 0xB2, 0x29, 0x3C, 0xDF, 0x5D, 0xF9, 0x1D,
        0x39, 0xED, 0x06, 0xAB, 0x2F, 0x98, 0x08, 0x10, 0x96, 0x56, 0xC4, 0x3A, 0x1E, 0x95, 0x2C, 0xB5
    };

    internal static string Host() => Decode(HostEnc);
    internal static int Port() => int.Parse(Decode(PortEnc));
    internal static byte[] JoinKey() => Encoding.UTF8.GetBytes(Decode(JoinEnc));

    internal static byte[] SessionKey()
    {
        using var hmac = new HMACSHA256(JoinKey());
        return hmac.ComputeHash(Encoding.UTF8.GetBytes("HNS-ZT-SESSION-V1"));
    }

    internal static string HelloProof(string clientId)
    {
        using var hmac = new HMACSHA256(JoinKey());
        return Convert.ToBase64String(hmac.ComputeHash(Encoding.UTF8.GetBytes("hello|" + clientId)));
    }

    private static string Decode(byte[] enc)
    {
        byte[] outBytes = new byte[enc.Length];
        for (int i = 0; i < enc.Length; i++)
            outBytes[i] = (byte)(enc[i] ^ K[i % K.Length]);
        return Encoding.UTF8.GetString(outBytes);
    }
}
