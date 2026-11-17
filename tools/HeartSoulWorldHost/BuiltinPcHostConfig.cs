using System.Security.Cryptography;
using System.Text;

namespace HeartSoulWorldHost;

/// <summary>
/// Baked ZeroTier endpoint + shared auth secret (XOR-obfuscated).
/// Never log or print decoded secret material.
/// </summary>
internal static class BuiltinPcHostConfig
{
    private static readonly byte[] K = {
        0xA7, 0x3C, 0x91, 0x5E, 0x28, 0xF4, 0x6B, 0xD0, 0x19, 0xC2, 0x7F, 0x44,
        0xE8, 0x33, 0xB5, 0x6A, 0x0D, 0x9F, 0x52, 0xC8, 0x17, 0xE1, 0x4A, 0x76
    };

    // 10.137.178.161
    private static readonly byte[] HostEnc = {
        0x96, 0x0C, 0xBF, 0x6F, 0x1B, 0xC3, 0x45, 0xE1, 0x2E, 0xFA, 0x51, 0x75, 0xDE, 0x02
    };
    // 27770
    private static readonly byte[] PortEnc = { 0x95, 0x0B, 0xA6, 0x69, 0x18 };
    // 32-byte shared auth secret (raw bytes, not a printable string).
    private static readonly byte[] SecretEnc = {
        0x76, 0x2B, 0xFF, 0x47, 0x3C, 0x11, 0xD9, 0xA9, 0x39, 0x80, 0xEA, 0x1B, 0xCB, 0x35, 0x64, 0x27,
        0xFC, 0x5C, 0x8E, 0x49, 0x81, 0xB1, 0x11, 0x14, 0x04, 0x85, 0x74, 0x67, 0xD1, 0x02, 0x7D, 0x20
    };

    internal static string Host() => DecodeString(HostEnc);
    internal static int Port() => int.Parse(DecodeString(PortEnc));
    internal static byte[] SharedSecret() => DecodeBytes(SecretEnc);

    internal static byte[] SessionKey()
    {
        using var hmac = new HMACSHA256(SharedSecret());
        return hmac.ComputeHash(Encoding.UTF8.GetBytes("HNS-ZT-SESSION-V1"));
    }

    internal static string HelloProof(string clientId)
    {
        using var hmac = new HMACSHA256(SharedSecret());
        return Convert.ToBase64String(hmac.ComputeHash(Encoding.UTF8.GetBytes("hello|" + clientId)));
    }

    private static byte[] DecodeBytes(byte[] enc)
    {
        byte[] outBytes = new byte[enc.Length];
        for (int i = 0; i < enc.Length; i++)
            outBytes[i] = (byte)(enc[i] ^ K[i % K.Length]);
        return outBytes;
    }

    private static string DecodeString(byte[] enc) => Encoding.UTF8.GetString(DecodeBytes(enc));
}
